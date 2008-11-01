#include "sysincludes.h"
#include "msdos.h"
#include "stream.h"
#include "file.h"
#include "mtoolsDirentry.h"
#include "file_name.h"

void initializeDirentry(direntry_t *entry, Stream_t *Dir)
{
	entry->entry = -1;
/*	entry->parent = getDirentry(Dir);*/
	entry->Dir = Dir;
	entry->beginSlot = 0;
	entry->endSlot = 0;
}

int isNotFound(direntry_t *entry)
{
	return entry->entry == -2;
}

direntry_t *getParent(direntry_t *entry)
{
	return getDirentry(entry->Dir);
}


static int getPathLen(direntry_t *entry)
{
	int length=0;

	while(1) {
		if(entry->entry == -3) /* rootDir */
			return length + 3;
		
		length += 1 + wcslen(entry->name);
		entry = getDirentry(entry->Dir);
	}
}

static char *sprintPwd(direntry_t *entry, char *ptr)
{
	if(entry->entry == -3) {
		*ptr++ = getDrive(entry->Dir);
		*ptr++ = ':';
		*ptr++ = '/';
	} else {
		ptr = sprintPwd(getDirentry(entry->Dir), ptr);
		if(ptr[-1] != '/')
			*ptr++ = '/';
		ptr += wchar_to_native(entry->name, ptr, MAX_VNAMELEN);
	}
	return ptr;		
}


#ifdef HAVE_WCHAR_H
#define NEED_ESCAPE L"\"$\\"
#else
#define NEED_ESCAPE "\"$\\"
#endif

static void _fprintPwd(FILE *f, direntry_t *entry, int recurs, int escape)
{
	if(entry->entry == -3) {
		putc(getDrive(entry->Dir), f);
		putc(':', f);
		if(!recurs)
			putc('/', f);
	} else {
		_fprintPwd(f, getDirentry(entry->Dir), 1, escape);
		if (escape && wcspbrk(entry->name, NEED_ESCAPE)) {
			wchar_t *ptr;
			putc('/', f);
			for(ptr = entry->name; *ptr; ptr++) {
				if (wcschr(NEED_ESCAPE, *ptr))
					putc('\\', f);
				putwc(*ptr, f);
			}
		} else {
			char tmp[4*MAX_VNAMELEN+1];
			wchar_to_native(entry->name,tmp,MAX_VNAMELEN);
			fprintf(f, "/%s", tmp);
		}
	}
}

void fprintPwd(FILE *f, direntry_t *entry, int escape)
{
	if (escape)
		putc('"', f);
	_fprintPwd(f, entry, 0, escape);
	if(escape)
		putc('"', f);
}

char *getPwd(direntry_t *entry)
{
	int size;
	char *ret;
	char *end;

	size = getPathLen(entry);
	ret = malloc(size+1);
	if(!ret)
		return 0;
	end = sprintPwd(entry, ret);
	*end = '\0';
	return ret;
}

int isSubdirOf(Stream_t *inside, Stream_t *outside)
{
	while(1) {
		if(inside == outside) /* both are the same */
			return 1;
		if(getDirentry(inside)->entry == -3) /* root directory */
			return 0;
		/* look further up */
		inside = getDirentry(inside)->Dir;
	}			
}
