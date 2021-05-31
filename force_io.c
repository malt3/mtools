/*  Copyright 1996,1997,1999,2001,2002,2009 Alain Knaff.
 *  This file is part of mtools.
 *
 *  Mtools is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Mtools is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Mtools.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Force I/O to be done to complete transfer length
 *
 * written by:
 *
 * Alain L. Knaff			
 * alain@knaff.lu
 *
 */

#include "sysincludes.h"
#include "msdos.h"
#include "stream.h"

static ssize_t force_io(Stream_t *Stream,
			char *buf, mt_off_t start, size_t len,
			ssize_t (*io)(Stream_t *, char *, mt_off_t, size_t))
{
	ssize_t ret;
	int done=0;
	
	while(len){
		ret = io(Stream, buf, start, len);
		if ( ret <= 0 ){
			if (done)
				return done;
			else
				return ret;
		}
		start += (size_t) ret;
		done += ret;
		len -= (size_t) ret;
		buf += ret;
	}
	return done;
}

ssize_t force_write(Stream_t *Stream, char *buf, mt_off_t start, size_t len)
{
	return force_io(Stream, buf, start, len,
			Stream->Class->write);
}

ssize_t force_read(Stream_t *Stream, char *buf, mt_off_t start, size_t len)
{
	return force_io(Stream, buf, start, len,
					Stream->Class->read);
}
