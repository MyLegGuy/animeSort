#include <stdio.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>
#include <libgen.h> // for basename
#include <argp.h>

#define SLASH '/'
#define ALIASLISTENABLED 1
#define GETEPISODEFILENAMECOMMAND "/usr/local/bin/getAnimeFilename"

void removeNewline(char* _toRemove);
// argument parse info
static error_t parse_opt (int key, char *arg, struct argp_state *state);
const char* argp_program_version = "animeSort "__DATE__" "__TIME__;
const char* argp_program_bug_address = "<supermoronslayer@gmail.com>";
const char documentation[] = "animeSort - sort anime files into subdirectories";
const char argDocumentation[] = "AnimeFolderRoot FILES...";
struct argp_option cmdOptions[] = { /* The options we understand. */
	{"simulate",'s',NULL,0,"Don't move any files. Print where they would've been moved."},
	{"mkdir",'m',NULL,0,"Allow creation of destination directory if it doesn't exist."},
	{"brave",'b',NULL,0,"Continue with other files even if one fails."},
	#if ALIASLISTENABLED == 1
	{"aliasList",'a',"FILE",0,"File with list of aliases."},
	{"aliasFilenames",'f',NULL,0,"Change the filename for aliased entries too."},
	{"aliasFilenameFail",'e',NULL,0,"Continue if aliasing a filename fails."},
	#endif
	{ 0 }
};
struct argp argp = { cmdOptions, parse_opt, argDocumentation, documentation };

char* animeFolderRoot=NULL;
char isSimulation=0;
char canMakeSubdirectory=0;
char exitOnFail=1;

#if ALIASLISTENABLED==1
char aliasFilenamesToo=0;
char failOnFilenameAliasFail=1;
struct aliasPair{
	char* src;
	char* dest;
};
size_t aliasNum;
struct aliasPair* aliases; // is sorted by the program
int compareAlias(const void* p1, const void* p2){
	return strcmp(((struct aliasPair*)p1)->src,((struct aliasPair*)p2)->src);
}
// same as compareAlias, but p1 is a string
int compareStrToAlias(const void* p1, const void* p2){
	return strcmp(p1,((struct aliasPair*)p2)->src);
}
char readAliasList(const char* _file){
	aliasNum=0;
	aliases=NULL;
	FILE* fp = fopen(_file,"rb");
	if (fp==NULL){
		return 1;
	}
	char _ret=0;
	size_t _curMaxArray=0;
	//
	char _isSrc=1;
	size_t _lineSize=0;
	char* _lastLine=NULL;
	while (1){
		errno=0;
		if (getline(&_lastLine,&_lineSize,fp)==-1){
			_ret=(errno!=0);
			break;
		}
		removeNewline(_lastLine);
		char* _addThis = strdup(_lastLine);
		if (_isSrc){ // read source
			if (aliasNum>=_curMaxArray){
				_curMaxArray+=10;
				aliases = realloc(aliases,sizeof(struct aliasPair)*_curMaxArray);
			}
			aliases[aliasNum].src=_addThis;
		}else{ // read dest
			aliases[aliasNum++].dest=_addThis;
		}
		_isSrc=!_isSrc;
	}
	fclose(fp);
	free(_lastLine);
	if (!_isSrc){
		fprintf(stderr,"alias file: source line without dest line\n");
	}else{
		qsort(aliases,aliasNum, sizeof(struct aliasPair),compareAlias);
	}
	return _ret;
}
#endif

FILE* goodpopen(char* const _args[]){
	int _crossFiles[2]; // File descriptors that work for both processes
	if (pipe(_crossFiles)!=0){
		return NULL;
	}
	pid_t _newProcess = fork();
	if (_newProcess==-1){
		close(_crossFiles[0]);
		close(_crossFiles[1]);
		return NULL;
	}else if (_newProcess==0){ // 0 is returned to the new process
		close(_crossFiles[0]); // Child does not need to read
		dup2(_crossFiles[1], STDOUT_FILENO); // make _crossFiles[1] be the same as STDOUT_FILENO
		// First arg is the path of the file again
		execv(_args[0],_args); // This will do this program and then end the child process
		exit(1); // This means execv failed
	}
	close(_crossFiles[1]); // Parent doesn't need to write
	FILE* _ret = fdopen(_crossFiles[0],"r");
	waitpid(_newProcess,NULL,0);
	return _ret;
}
void removeNewline(char* _toRemove){ // POSIX says all lines end in line break, but i check anyway
	int _cachedStrlen = strlen(_toRemove);
	if (_cachedStrlen==0){
		return;
	}
	if (_toRemove[_cachedStrlen-1]=='\n'){ // Last char is UNIX newline
		_toRemove[_cachedStrlen-1]='\0';
	}
}
char dirExists(const char* _checkHere){
	struct stat sb;
	return (stat(_checkHere,&sb)==0 && S_ISDIR(sb.st_mode));
}
char sort(const char* _sortThis, const char* _rootAnimeFolder){
	errno=0;
	if (!isSimulation && access(_sortThis,F_OK)){
		fprintf(stderr,"%s check error: %s\n",_sortThis,strerror(errno));
		return 1;
	}
	char _ret=0;
	char* _specificAnimeFolder=NULL;
	// get filename of passed file
	char* _usableSortThis = strdup(_sortThis);
	char* _basename = basename(_usableSortThis);
	int _basenameLen = strlen(_basename);
	if (_basenameLen==0){
		fprintf(stderr,"failed to get filename of %s\n",_sortThis);
		_ret=1;
		goto freelocal;
	}
	// run the program to get the info
	char* const _programArgs[] = {
		GETEPISODEFILENAMECOMMAND,
		(char*)_sortThis,
		NULL,
	};
	FILE* _programOut = goodpopen(_programArgs);
	if (!_programOut){
		fprintf(stderr,"failed to open process\n");
		return 1;
	}
	size_t _lineSize=0;
	char* _lastLine=NULL;
	// first line is group, useless.
	if (getline(&_lastLine,&_lineSize,_programOut)==-1){
		fprintf(stderr,"failed to skip group line\n");
		_ret=1;
		goto freelocal;
	}
	// grab title on the second line
	if (getline(&_lastLine,&_lineSize,_programOut)==-1){
		fprintf(stderr,"error reading title line\n");
		_ret=1;
		goto freelocal;
	}
	removeNewline(_lastLine);
	if (strlen(_lastLine)<=1){
		fprintf(stderr,"title line is too small. invalid.\n");
		_ret=1;
		goto freelocal;
	}
	#if ALIASLISTENABLED == 1
	// alias title if allowed
	struct aliasPair* _foundAlias = bsearch(_lastLine,aliases,aliasNum,sizeof(struct aliasPair),compareStrToAlias);
	if (_foundAlias!=NULL){
		free(_lastLine);
		_lastLine=strdup(_foundAlias->dest);
		if (aliasFilenamesToo){
			char* _titleSubstring = strstr(_basename,_foundAlias->src);
			if (_titleSubstring!=NULL){
				int _preFilenameBytes = _titleSubstring-_basename;
				char* _postFilename=_titleSubstring+strlen(_foundAlias->src);
				int _postFilenameBytes = strlen(_postFilename);
				
				char* _newBasename = malloc(_preFilenameBytes+strlen(_foundAlias->dest)+_postFilenameBytes+1);
				memcpy(_newBasename,_basename,_preFilenameBytes);
				strcpy(&(_newBasename[_preFilenameBytes]),_foundAlias->dest);
				strcat(_newBasename,_postFilename);

				free(_usableSortThis);
				_usableSortThis=_newBasename;
				_basename=_newBasename;
			}else{
				if (failOnFilenameAliasFail){
					fprintf(stderr,"Failed to alias filename %s to %s from %s\n",_basename,_foundAlias->dest,_foundAlias->src);
					_ret=1;
				}
			}
		}
	}
	#endif
	// create full folder path
	_specificAnimeFolder = malloc(strlen(_rootAnimeFolder)+strlen(_lastLine)+strlen(_sortThis)+2);
	strcpy(_specificAnimeFolder,_rootAnimeFolder);
	strcat(_specificAnimeFolder,_lastLine);
	// to reduce chance of copying to the wrong place, don't allow dest folder that doesn't exist
    struct stat sb;
    if (!dirExists(_specificAnimeFolder)){
		if (canMakeSubdirectory){
			if (!isSimulation){
				errno=0;
				if (mkdir(_specificAnimeFolder,700)){ // true chads don't let anybody else touch their stuff
					fprintf(stderr,"makedir %s: %s\n",_specificAnimeFolder,strerror(errno));
					_ret=1;
					goto freelocal;
				}
			}else{
				printf("mkdir:\n\t%s\n",_specificAnimeFolder);
			}
		}else{
			fprintf(stderr,"Directory not exist, please make it.\n\t%s\n",_specificAnimeFolder);
			_ret=1;
			goto freelocal;
		}
    }
	
	// make the full folder path into full file path
	int _folderLen = strlen(_specificAnimeFolder);
	_specificAnimeFolder[_folderLen]=SLASH;
	memcpy(&(_specificAnimeFolder[_folderLen+1]),_basename,_basenameLen);
	_specificAnimeFolder[_folderLen+1+_basenameLen]='\0';
	// do the move
	if (!isSimulation){
		errno=0;
		// check if the file already exists so we don't overwrite it
		if (access(_specificAnimeFolder,F_OK)==0){
			fprintf(stderr,"%s already exists\n",_specificAnimeFolder);
			_ret=1;
			goto freelocal;
		}
		if (rename(_sortThis,_specificAnimeFolder)){
			fprintf(stderr,"failed to move %s to %s: %s\n",_sortThis,_specificAnimeFolder,strerror(errno));
			_ret=1;
			goto freelocal;
		}
	}else{
		printf("Move:\n\t%s\n\t%s\n",_sortThis,_specificAnimeFolder);
	}
freelocal:
	fclose(_programOut);
	free(_specificAnimeFolder);
	free(_lastLine);
	free(_usableSortThis);
	return _ret;
}
///////////
static error_t parse_opt (int key, char *arg, struct argp_state *state){
	switch (key){
		case 's':
			isSimulation=1;
			break;
		case 'm':
			canMakeSubdirectory=1;
			break;
		case 'b':
			exitOnFail=0;
			break;
		#if ALIASLISTENABLED == 1
		case 'a':
			if (readAliasList(arg)){
				fprintf(stderr,"failed to read arg list %s\n",arg);
				exit(1);
			}
			break;
		case 'f':
			aliasFilenamesToo=1;
			break;
		case 'e':
			failOnFilenameAliasFail=0;
			break;
		#endif
		case ARGP_KEY_ARG:
			if (state->arg_num==0){
				int _pathLen = strlen(arg);
				if (arg[_pathLen-1]!=SLASH){
					animeFolderRoot=malloc(_pathLen+2);
					memcpy(animeFolderRoot,arg,_pathLen);
					animeFolderRoot[_pathLen]=SLASH;
					animeFolderRoot[_pathLen+1]='\0';
				}else{
					animeFolderRoot=arg;
				}
			}else{
				if (sort(arg,animeFolderRoot) && exitOnFail){
					exit(1);
				}
			}			
			break;
		case ARGP_KEY_END:
			if (state->arg_num < 2){
				fprintf(stderr,"you need at least the anime folder and a single file\n");
				argp_usage (state);
			}
			break;
		default:
			return ARGP_ERR_UNKNOWN;
    }
	return 0;
}
int main(int argc, char** argv){
	argp_parse (&argp, argc, argv, 0, 0, NULL);
}
