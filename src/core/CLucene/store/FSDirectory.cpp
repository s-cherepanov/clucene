/*------------------------------------------------------------------------------
* Copyright (C) 2003-2006 Ben van Klinken and the CLucene Team
* 
* Distributable under the terms of either the Apache License (Version 2.0) or 
* the GNU Lesser General Public License, as specified in the COPYING file.
------------------------------------------------------------------------------*/
#include "CLucene/_ApiHeader.h"

#include <fcntl.h>
#ifdef _CL_HAVE_IO_H
	#include <io.h>
#endif
#ifdef _CL_HAVE_SYS_STAT_H
	#include <sys/stat.h>
#endif
#ifdef _CL_HAVE_UNISTD_H
	#include <unistd.h>
#endif
#ifdef _CL_HAVE_DIRECT_H
	#include <direct.h>
#endif
#include <errno.h>

#include "FSDirectory.h"
#include "LockFactory.h"
#include "CLucene/index/IndexReader.h"
#include "CLucene/index/IndexWriter.h"
#include "CLucene/util/Misc.h"
#include "CLucene/util/_MD5Digester.h"
#include "CLucene/util/dirent.h" //if we have dirent, then the native one will be used

#ifdef LUCENE_FS_MMAP
    #include "_MMap.h"
#endif

CL_NS_DEF(store)
CL_NS_USE(util)

   /** This cache of directories ensures that there is a unique Directory
   * instance per path, so that synchronization on the Directory can be used to
   * synchronize access between readers and writers.
   */
	static CL_NS(util)::CLHashMap<const char*,FSDirectory*,CL_NS(util)::Compare::Char,CL_NS(util)::Equals::Char> DIRECTORIES(false,false);

	bool FSDirectory::disableLocks=false;


	class FSDirectory::FSIndexInput:public BufferedIndexInput {
		/**
		* We used a shared handle between all the fsindexinput clones.
		* This reduces number of file handles we need, and it means
		* we dont have to use file tell (which is slow) before doing
		* a read.
		*/
		class SharedHandle: LUCENE_REFBASE{
		public:
			int32_t fhandle;
			int64_t _length;
			int64_t _fpos;
			DEFINE_MUTEX(THIS_LOCK)
			char path[CL_MAX_DIR]; //todo: this is only used for cloning, better to get information from the fhandle
			SharedHandle();
			~SharedHandle();
		};
		SharedHandle* handle;
		int64_t _pos;
	protected:
		FSIndexInput(const FSIndexInput& clone);
	public:
		FSIndexInput(const char* path, int32_t bufferSize=CL_NS(store)::BufferedIndexOutput::BUFFER_SIZE);
		~FSIndexInput();

		IndexInput* clone() const;
		void close();
		int64_t length() const { return handle->_length; }
		
		const char* getDirectoryType() const{ return FSDirectory::DirectoryType(); }
	protected:
		// Random-access methods 
		void seekInternal(const int64_t position);
		// IndexInput methods 
		void readInternal(uint8_t* b, const int32_t len);
	};

	class FSDirectory::FSIndexOutput: public BufferedIndexOutput {
	private:
		int32_t fhandle;
	protected:
		// output methods: 
		void flushBuffer(const uint8_t* b, const int32_t size);
	public:
		FSIndexOutput(const char* path);
		~FSIndexOutput();

		// output methods:
		void close();

		// Random-access methods 
		void seek(const int64_t pos);
		int64_t length() const;
	};

	FSDirectory::FSIndexInput::FSIndexInput(const char* path, int32_t __bufferSize):
		BufferedIndexInput(__bufferSize)	
    {
	//Func - Constructor.
	//       Opens the file named path
	//Pre  - path != NULL
	//Post - if the file could not be opened  an exception is thrown.

	  CND_PRECONDITION(path != NULL, "path is NULL");

	  handle = _CLNEW SharedHandle();
	  strcpy(handle->path,path);

	  //Open the file
	  handle->fhandle  = _open(path, _O_BINARY | O_RDONLY | _O_RANDOM, _S_IREAD );
	  
	  //Check if a valid handle was retrieved
	  if (handle->fhandle < 0){
		int err = errno;
        if ( err == ENOENT )
		    _CLTHROWA(CL_ERR_IO, "File does not exist");
        else if ( err == EACCES )
            _CLTHROWA(CL_ERR_IO, "File Access denied");
        else if ( err == EMFILE )
            _CLTHROWA(CL_ERR_IO, "Too many open files");
	  }

	  //Store the file length
	  handle->_length = fileSize(handle->fhandle);
	  handle->_fpos = 0;
	  this->_pos = 0;
  }

  FSDirectory::FSIndexInput::FSIndexInput(const FSIndexInput& other): BufferedIndexInput(other){
  //Func - Constructor
  //       Uses clone for its initialization
  //Pre  - clone is a valide instance of FSIndexInput
  //Post - The instance has been created and initialized by clone
	if ( other.handle == NULL )
		_CLTHROWA(CL_ERR_NullPointer, "other handle is null");

	SCOPED_LOCK_MUTEX(other.handle->THIS_LOCK)
	handle = _CL_POINTER(other.handle);
	_pos = other.handle->_fpos; //note where we are currently...
  }

  FSDirectory::FSIndexInput::SharedHandle::SharedHandle(){
  	fhandle = 0;
    _length = 0;
    _fpos = 0;
    path[0]=0;
  }
  FSDirectory::FSIndexInput::SharedHandle::~SharedHandle() {
    if ( fhandle >= 0 ){
      if ( ::_close(fhandle) != 0 )
        _CLTHROWA(CL_ERR_IO, "File IO Close error");
      else
        fhandle = -1;
    }
  }

  FSDirectory::FSIndexInput::~FSIndexInput(){
  //Func - Destructor
  //Pre  - True
  //Post - The file for which this instance is responsible has been closed.
  //       The instance has been destroyed

	  FSIndexInput::close();
  }

  IndexInput* FSDirectory::FSIndexInput::clone() const
  {
    return _CLNEW FSDirectory::FSIndexInput(*this);
  }
  void FSDirectory::FSIndexInput::close()  {
	BufferedIndexInput::close();
	_CLDECDELETE(handle);
  }

  void FSDirectory::FSIndexInput::seekInternal(const int64_t position)  {
	CND_PRECONDITION(position>=0 &&position<handle->_length,"Seeking out of range")
	_pos = position;
  }

/** IndexInput methods */
void FSDirectory::FSIndexInput::readInternal(uint8_t* b, const int32_t len) {
	SCOPED_LOCK_MUTEX(handle->THIS_LOCK)
	CND_PRECONDITION(handle!=NULL,"shared file handle has closed");
	CND_PRECONDITION(handle->fhandle>=0,"file is not open");

	if ( handle->_fpos != _pos ){
		if ( fileSeek(handle->fhandle,_pos,SEEK_SET) != _pos ){
			_CLTHROWA( CL_ERR_IO, "File IO Seek error");
		}
		handle->_fpos = _pos;
	}

	bufferLength = _read(handle->fhandle,b,len); // 2004.10.31:SF 1037836
	if (bufferLength == 0){
		_CLTHROWA(CL_ERR_IO, "read past EOF");
	}
	if (bufferLength == -1){
		//if (EINTR == errno) we could do something else... but we have
		//to guarantee some return, or throw EOF
		
		_CLTHROWA(CL_ERR_IO, "read error");
	}
	_pos+=bufferLength;
	handle->_fpos=_pos;
}

  FSDirectory::FSIndexOutput::FSIndexOutput(const char* path){
	//O_BINARY - Opens file in binary (untranslated) mode
	//O_CREAT - Creates and opens new file for writing. Has no effect if file specified by filename exists
	//O_RANDOM - Specifies that caching is optimized for, but not restricted to, random access from disk.
	//O_WRONLY - Opens file for writing only;
	if ( Misc::dir_Exists(path) )
	  fhandle = _open( path, _O_BINARY | O_RDWR | _O_RANDOM | O_TRUNC, _S_IREAD | _S_IWRITE);
	else // added by JBP
	  fhandle = _open( path, _O_BINARY | O_RDWR | _O_RANDOM | O_CREAT, _S_IREAD | _S_IWRITE);

	if ( fhandle < 0 ){
        int err = errno;
        if ( err == ENOENT )
    	    _CLTHROWA(CL_ERR_IO, "File does not exist");
        else if ( err == EACCES )
            _CLTHROWA(CL_ERR_IO, "File Access denied");
        else if ( err == EMFILE )
            _CLTHROWA(CL_ERR_IO, "Too many open files");
    }
  }
  FSDirectory::FSIndexOutput::~FSIndexOutput(){
	if ( fhandle >= 0 ){
	  try {
        FSIndexOutput::close();
	  }catch(CLuceneError& err){
	    //ignore IO errors...
	    if ( err.number() != CL_ERR_IO )
	        throw;
	  }
	}
  }

  /** output methods: */
  void FSDirectory::FSIndexOutput::flushBuffer(const uint8_t* b, const int32_t size) {
	  CND_PRECONDITION(fhandle>=0,"file is not open");
      if ( size > 0 && _write(fhandle,b,size) != size )
        _CLTHROWA(CL_ERR_IO, "File IO Write error");
  }
  void FSDirectory::FSIndexOutput::close() {
    try{
      BufferedIndexOutput::close();
    }catch(CLuceneError& err){
	    //ignore IO errors...
	    if ( err.number() != CL_ERR_IO )
	        throw;
    }

    if ( ::_close(fhandle) != 0 )
      _CLTHROWA(CL_ERR_IO, "File IO Close error");
    else
      fhandle = -1; //-1 now indicates closed
  }

  void FSDirectory::FSIndexOutput::seek(const int64_t pos) {
    CND_PRECONDITION(fhandle>=0,"file is not open");
    BufferedIndexOutput::seek(pos);
	int64_t ret = fileSeek(fhandle,pos,SEEK_SET);
	if ( ret != pos ){
      _CLTHROWA(CL_ERR_IO, "File IO Seek error");
	}
  }
  int64_t FSDirectory::FSIndexOutput::length() const {
	  CND_PRECONDITION(fhandle>=0,"file is not open");
	  return fileSize(fhandle);
  }


	const char* FSDirectory::LOCK_DIR=NULL;
	const char* FSDirectory::getLockDir(){
		#ifdef LUCENE_LOCK_DIR
		LOCK_DIR = LUCENE_LOCK_DIR;
		#else	
			#ifdef LUCENE_LOCK_DIR_ENV_1
			if ( LOCK_DIR == NULL )
				LOCK_DIR = getenv(LUCENE_LOCK_DIR_ENV_1);
			#endif			
			#ifdef LUCENE_LOCK_DIR_ENV_2
			if ( LOCK_DIR == NULL )
				LOCK_DIR = getenv(LUCENE_LOCK_DIR_ENV_2);
			#endif
			#ifdef LUCENE_LOCK_DIR_ENV_FALLBACK
			if ( LOCK_DIR == NULL )
				LOCK_DIR=LUCENE_LOCK_DIR_ENV_FALLBACK;
			#endif
			if ( LOCK_DIR == NULL )
				_CLTHROWA(CL_ERR_IO, "Couldn't get determine lock dir");
		#endif
		
		return LOCK_DIR;
	}

  FSDirectory::FSDirectory(const char* path, const bool createDir, LockFactory* lockFactory):
   Directory(),
   directory(_CL_NEWARRAY(char,CL_MAX_PATH)),
   lockDir(_CL_NEWARRAY(char,CL_MAX_PATH)),
   refCount(0),
   useMMap(false)
  {
  	_realpath(path,directory);//set a realpath so that if we change directory, we can still function
  	if ( !directory || !*directory ){
  		strcpy(directory,path);	
  	}
        
    bool doClearLockID = false;
    
    if ( lockFactory == NULL ) {
    	if ( disableLocks ) {
    		lockFactory = NoLockFactory::getNoLockFactory();
    	} else {
    		lockFactory = _CLNEW FSLockFactory( path );
    		doClearLockID = true;
    	}
    }
    
    setLockFactory( lockFactory );
    
    if ( doClearLockID ) {
    	lockFactory->setLockPrefix(NULL);
    }

    if (createDir) {
      create();
    }

    if (!Misc::dir_Exists(directory)){
		char* err = _CL_NEWARRAY(char,19+strlen(path)+1); //19: len of " is not a directory"
		strcpy(err,path);
		strcat(err," is not a directory");
        _CLTHROWA_DEL(CL_ERR_IO, err );
    }

  }


  void FSDirectory::create(){
    SCOPED_LOCK_MUTEX(THIS_LOCK)
		struct fileStat fstat;
    if ( fileStat(directory,&fstat) != 0 ) {
	  	//todo: should construct directory using _mkdirs... have to write replacement
      if ( _mkdir(directory) == -1 ){
			  char* err = _CL_NEWARRAY(char,27+strlen(directory)+1); //27: len of "Couldn't create directory: "
			  strcpy(err,"Couldn't create directory: ");
			  strcat(err,directory);
			  _CLTHROWA_DEL(CL_ERR_IO, err );
      }
		}

		if ( fileStat(directory,&fstat) != 0 || !(fstat.st_mode & S_IFDIR) ){
	      char tmp[1024];
	      _snprintf(tmp,1024,"%s not a directory", directory);
	      _CLTHROWA(CL_ERR_IO,tmp);
		}

	  //clear old files
    DIR* dir = opendir(directory);
    struct dirent* fl = readdir(dir);
    struct fileStat buf;

    char path[CL_MAX_DIR];
    while ( fl != NULL ){
		  if ( CL_NS(index)::IndexReader::isLuceneFile(fl->d_name) ){
				_snprintf(path,CL_MAX_DIR,"%s/%s",directory,fl->d_name);
				int32_t ret = fileStat(path,&buf);
				if ( ret==0 && !(buf.st_mode & S_IFDIR) ) {
					if ( (strcmp(fl->d_name, ".")) && (strcmp(fl->d_name, "..")) ) {
						if ( _unlink( path ) == -1 ) {
						  closedir(dir);
						  _CLTHROWA(CL_ERR_IO, "Couldn't delete file "); //todo: make richer error
						}
					}
				}
		  }
		  fl = readdir(dir);
    }
    closedir(dir);

    lockFactory->clearLock( CL_NS(index)::IndexWriter::WRITE_LOCK_NAME );
    
  }

  void FSDirectory::priv_getFN(char* buffer, const char* name) const{
      buffer[0] = 0;
      strcpy(buffer,directory);
      strcat(buffer, PATH_DELIMITERA );
      strcat(buffer,name);
  }

  FSDirectory::~FSDirectory(){
	  _CLDELETE( lockFactory );
	  _CLDELETE_CaARRAY(directory);
	  _CLDELETE_CaARRAY(lockDir);
  }
  

    void FSDirectory::setUseMMap(bool value){ useMMap = value; }
    bool FSDirectory::getUseMMap() const{ return useMMap; }
    const char* FSDirectory::DirectoryType(){ return "FS"; }
    const char* FSDirectory::getDirectoryType() const{ return "FS"; }
    void FSDirectory::setDisableLocks(bool doDisableLocks) { disableLocks = doDisableLocks; }
    bool FSDirectory::getDisableLocks() { return disableLocks; }


  void FSDirectory::list(vector<string>* names) const{ //todo: fix this, ugly!!!
    CND_PRECONDITION(directory[0]!=0,"directory is not open");
    DIR* dir = opendir(directory);
    
    struct dirent* fl = readdir(dir);
    struct fileStat buf;

    char path[CL_MAX_DIR];
	strncpy(path,directory,CL_MAX_DIR);
    strcat(path,PATH_DELIMITERA);
    char* pathP = path + strlen(path);

    while ( fl != NULL ){
      strcpy(pathP,fl->d_name);
      fileStat(path,&buf);
      if ( !(buf.st_mode & S_IFDIR) ) {
        names->push_back( fl->d_name );
      }
      fl = readdir(dir);
    }
    closedir(dir);
  }

  bool FSDirectory::fileExists(const char* name) const {
	  CND_PRECONDITION(directory[0]!=0,"directory is not open");
    char fl[CL_MAX_DIR];
    priv_getFN(fl, name);
    return Misc::dir_Exists( fl );
  }

  const char* FSDirectory::getDirName() const{
    return directory;
  }

  //static
  FSDirectory* FSDirectory::getDirectory(const char* file, const bool _create, LockFactory* lockFactory){
    FSDirectory* dir = NULL;
	{
		if ( !file || !*file )
			_CLTHROWA(CL_ERR_IO,"Invalid directory");

		SCOPED_LOCK_MUTEX(DIRECTORIES.THIS_LOCK)
		dir = DIRECTORIES.get(file);
		if ( dir == NULL  ){
			dir = _CLNEW FSDirectory(file,_create,lockFactory);
			DIRECTORIES.put( dir->directory, dir);
		} else if ( _create ) {
	    	dir->create();
		} else {
			if ( lockFactory != NULL && lockFactory != dir->getLockFactory() ) {
				_CLTHROWA(CL_ERR_IO,"Directory was previously created with a different LockFactory instance, please pass NULL as the lockFactory instance and use setLockFactory to change it");
			}
		}

		{
			SCOPED_LOCK_MUTEX(dir->THIS_LOCK)
				dir->refCount++;
		}
	}

    return _CL_POINTER(dir);
  }

  int64_t FSDirectory::fileModified(const char* name) const {
	CND_PRECONDITION(directory[0]!=0,"directory is not open");
    struct fileStat buf;
    char buffer[CL_MAX_DIR];
    priv_getFN(buffer,name);
    if (fileStat( buffer, &buf ) == -1 )
      return 0;
    else
      return buf.st_mtime;
  }

  //static
  int64_t FSDirectory::fileModified(const char* dir, const char* name){
    struct fileStat buf;
    char buffer[CL_MAX_DIR];
	_snprintf(buffer,CL_MAX_DIR,"%s%s%s",dir,PATH_DELIMITERA,name);
    fileStat( buffer, &buf );
    return buf.st_mtime;
  }

  void FSDirectory::touchFile(const char* name){
	  CND_PRECONDITION(directory[0]!=0,"directory is not open");
    char buffer[CL_MAX_DIR];
    _snprintf(buffer,CL_MAX_DIR,"%s%s%s",directory,PATH_DELIMITERA,name);
	
    int32_t r = _open(buffer, O_RDWR, _S_IWRITE);
	if ( r < 0 )
		_CLTHROWA(CL_ERR_IO,"IO Error while touching file");
	::_close(r);
  }

  int64_t FSDirectory::fileLength(const char* name) const {
	  CND_PRECONDITION(directory[0]!=0,"directory is not open");
    struct fileStat buf;
    char buffer[CL_MAX_DIR];
    priv_getFN(buffer,name);
    if ( fileStat( buffer, &buf ) == -1 )
      return 0;
    else
      return buf.st_size;
  }

  IndexInput* FSDirectory::openInput(const char* name ) {
  	return openInput(name, CL_NS(store)::BufferedIndexOutput::BUFFER_SIZE);
  }
  
  IndexInput* FSDirectory::openMMapFile(const char* name, int32_t bufferSize){
#ifdef LUCENE_FS_MMAP
    char fl[CL_MAX_DIR];
    priv_getFN(fl, name);
	if ( Misc::file_Size(fl) < LUCENE_INT32_MAX_SHOULDBE ) //todo: would this be bigger on 64bit systems?. i suppose it would be...test first
		return _CLNEW MMapIndexInput( fl );
	else
		return _CLNEW FSIndexInput( fl, bufferSize );
#else
	_CLTHROWA(CL_ERR_Runtime,"MMap not enabled at compilation");
#endif
  }

  IndexInput* FSDirectory::openInput(const char* name, int32_t bufferSize ){
	CND_PRECONDITION(directory[0]!=0,"directory is not open")
    char fl[CL_MAX_DIR];
    priv_getFN(fl, name);
#ifdef LUCENE_FS_MMAP
	//todo: do some tests here... like if the file
	//is >2gb, then some system cannot mmap the file
	//also some file systems mmap will fail?? could detect here too
	if ( useMMap && Misc::file_Size(fl) < LUCENE_INT32_MAX_SHOULDBE ) //todo: would this be bigger on 64bit systems?. i suppose it would be...test first
		return _CLNEW MMapIndexInput( fl );
	else
#endif
	return _CLNEW FSIndexInput( fl, bufferSize );
  }
		
  void FSDirectory::close(){
    SCOPED_LOCK_MUTEX(DIRECTORIES.THIS_LOCK)
    {
	    SCOPED_LOCK_MUTEX(THIS_LOCK)
	
	    CND_PRECONDITION(directory[0]!=0,"directory is not open");
	
	    if (--refCount <= 0 ) {//refcount starts at 1
	        Directory* dir = DIRECTORIES.get(getDirName());
	        if(dir){
	            DIRECTORIES.remove( getDirName() ); //this will be removed in ~FSDirectory
	            _CLDECDELETE(dir);
	        }
	    }
	}
   }

   /**
   * So we can do some byte-to-hexchar conversion below
   */
	char HEX_DIGITS[] =
	{'0','1','2','3','4','5','6','7','8','9','a','b','c','d','e','f'};

	char* FSDirectory::getLockPrefix() const{
		char dirName[CL_MAX_PATH]; // name to be hashed
		if ( _realpath(directory,dirName) == NULL ){
			_CLTHROWA(CL_ERR_Runtime,"Invalid directory path");
		}

		//to make a compatible name with jlucene, we need to make some changes...
		if ( dirName[1] == ':' )
			dirName[0] = (char)_totupper((char)dirName[0]);

		char* smd5 = MD5String(dirName);

		char* ret=_CL_NEWARRAY(char,32+7+1); //32=2*16, 7=strlen("lucene-")
		strcpy(ret,"lucene-");
		strcat(ret,smd5);
		
		_CLDELETE_CaARRAY(smd5);

	    return ret; 
  }

  bool FSDirectory::doDeleteFile(const char* name)  {
	CND_PRECONDITION(directory[0]!=0,"directory is not open");
    char fl[CL_MAX_DIR];
    priv_getFN(fl, name);
	return _unlink(fl) != -1;
  }
  
  void FSDirectory::renameFile(const char* from, const char* to){
	CND_PRECONDITION(directory[0]!=0,"directory is not open");
    SCOPED_LOCK_MUTEX(THIS_LOCK)
    char old[CL_MAX_DIR];
    priv_getFN(old, from);

    char nu[CL_MAX_DIR];
    priv_getFN(nu, to);

    /* This is not atomic.  If the program crashes between the call to
    delete() and the call to renameTo() then we're screwed, but I've
    been unable to figure out how else to do this... */

    if ( Misc::dir_Exists(nu) ){
      //we run this sequence of unlinking an arbitary 100 times
      //on some platforms (namely windows), there can be a
      //delay between unlink and dir_exists==false          
      while ( true ){
          if( _unlink(nu) != 0 ){
    	    char* err = _CL_NEWARRAY(char,16+strlen(to)+1); //16: len of "couldn't delete "
    		strcpy(err,"couldn't delete ");
    		strcat(err,to);
            _CLTHROWA_DEL(CL_ERR_IO, err );
          }
          //we can wait until the dir_Exists() returns false
          //after the success run of unlink()
          int i=0;
		  while ( Misc::dir_Exists(nu) && i < 100 ){
			  if ( ++i > 50 ) //if it still doesn't show up, then we do some sleeping for the last 50ms
				  _LUCENE_SLEEP(1);
		  }
          if ( !Misc::dir_Exists(nu) )
            break; //keep trying to unlink until the file is gone, or the unlink fails.
      }
    }
    if ( _rename(old,nu) != 0 ){
       //todo: jlucene has some extra rename code - if the rename fails, it copies
       //the whole file to the new file... might want to implement that if renaming
       //fails on some platforms
        char buffer[20+CL_MAX_PATH+CL_MAX_PATH];
        strcpy(buffer,"couldn't rename ");
        strcat(buffer,from);
        strcat(buffer," to ");
        strcat(buffer,nu);
      _CLTHROWA(CL_ERR_IO, buffer );
    }
  }

  IndexOutput* FSDirectory::createOutput(const char* name) {
	CND_PRECONDITION(directory[0]!=0,"directory is not open");
    char fl[CL_MAX_DIR];
    priv_getFN(fl, name);
	if ( Misc::dir_Exists(fl) ){
		if ( _unlink(fl) != 0 ){
			char tmp[1024];
			strcpy(tmp, "Cannot overwrite: ");
			strcat(tmp, name);
			_CLTHROWA(CL_ERR_IO, tmp);
		}
	}
    return _CLNEW FSIndexOutput( fl );
  }

  TCHAR* FSDirectory::toString() const{
	  TCHAR* ret = _CL_NEWARRAY(TCHAR, strlen(this->directory) + 13); //strlen("FSDirectory@")
	  _tcscpy(ret,_T("FSDirectory@"));
	  STRCPY_AtoT(ret+12,directory,strlen(directory)+1);

	  return ret;
  }

CL_NS_END
