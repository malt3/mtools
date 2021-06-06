/*  Copyright 2021 Alain Knaff.
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
 * Open filesystem image or device, and push any remapping and/or partitioning layers on it
Buffer read/write module
 */

#include "sysincludes.h"
#include "msdos.h"
#include "mtools.h"

#include "open_image.h"

#include "plain_io.h"
#include "floppyd_io.h"
#include "xdf_io.h"
#include "scsi_io.h"
#include "remap.h"
#include "partition.h"
#include "offset.h"
#include "swap.h"

/*
 * Open filesystem image
 *   dev: device descriptor to use
 *   maxSize: if set, max size will be returned here
 *   geomFailureP: if set, geometry failure will be returned here. This means
 *     that caller should retry again opening the same image read/write
 *   skip: a bitmask of intermediary layers to skip
 *   errmsg: any error messages will be returned here
 */
Stream_t *OpenImage(struct device *out_dev, struct device *dev,
		    const char *name, int mode, char *errmsg,
		    int mode2, int lockMode,
		    mt_size_t *maxSize, int *geomFailureP,
		    int skip
#ifdef USE_XDF
		    , struct xdf_info *xdf_info
#endif
		    )
{
	Stream_t *Stream=NULL;
	int geomFailure=0;
	if(out_dev->misc_flags & FLOPPYD_FLAG) {
#ifdef USE_FLOPPYD
		Stream = FloppydOpen(out_dev, name, mode,
				     errmsg, maxSize);
#endif
	} else {

#ifdef USE_XDF
		Stream = XdfOpen(out_dev, name, mode, errmsg, xdf_info);
		if(Stream) {
			out_dev->use_2m = 0x7f;
			if(maxSize)
				*maxSize = (mt_size_t) max_off_t_31;
		}
#endif

		if (!Stream) {
			Stream = OpenScsi(out_dev, name,
					  mode,
					  errmsg, mode2, 0,
					  lockMode,
					  maxSize);
		}

		if (!Stream) {
			Stream = SimpleFileOpenWithLm(out_dev, dev, name,
						      mode,
						      errmsg, mode2, 0,
						      lockMode,
						      maxSize,
						      &geomFailure);
		}

		if(geomFailure) {
			if(*geomFailureP)
				*geomFailureP=geomFailure;
			return NULL;
		}
	}

	if( !Stream)
		return NULL;

	if(dev->data_map) {
		Stream_t *Remapped = Remap(Stream, dev->data_map, errmsg);
		if(Remapped == NULL) {
			FREE(&Stream);
			return NULL;
		}
		Stream = Remapped;
	}

	if(dev->offset) {
		Stream_t *Offset = OpenOffset(Stream, dev, dev->offset,
					      errmsg, maxSize);
		if(Offset == NULL) {
			FREE(&Stream);
			return NULL;
		}
		Stream = Offset;
	}

	if(DO_SWAP(dev)) {
		Stream_t *Swap = OpenSwap(Stream);
		if(Swap == NULL) {
			FREE(&Stream);
			return NULL;
		}
		Stream = Swap;
	}
	
	if(dev->partition && !(skip & SKIP_PARTITION)) {
		Stream_t *Partition = OpenPartition(Stream, out_dev,
						    errmsg, maxSize);
		if(Partition == NULL) {
			FREE(&Stream);
			return NULL;
		}
		Stream = Partition;
	}
	
	return Stream;
}

