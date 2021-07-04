/*  Copyright 1986-1992 Emmet P. Gray.
 *  Copyright 1994,1996-2009 Alain Knaff.
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
 * mformat.c
 */

#define DONT_NEED_WAIT

#include "sysincludes.h"
#include "msdos.h"
#include "mtools.h"
#include "mainloop.h"
#include "fsP.h"
#include "file.h"
#include "plain_io.h"
#include "nameclash.h"
#include "buffer.h"
#ifdef HAVE_ASSERT_H
#include <assert.h>
#endif
#include "stream.h"
#include "partition.h"
#include "open_image.h"
#include "file_name.h"
#include "lba.h"

#ifdef OS_linux
#include "linux/hdreg.h"
#include "linux/fs.h"

#endif

/**
 * Narrow down quantity of sectors to 32bit quantity, and bail out if
 * it doesn't fit in 32 bits
 */
static uint32_t mt_off_t_to_sectors(mt_off_t raw_sect) {
	/* Number of sectors must fit into 32bit value */
	if (raw_sect > UINT32_MAX) {
		fprintf(stderr, "Too many sectors for FAT %08x%08x\n",
			(uint32_t)(raw_sect>>32), (uint32_t)raw_sect);
		exit(1);
	}
	return (uint32_t) raw_sect;
}


static uint16_t init_geometry_boot(union bootsector *boot, struct device *dev,
				   uint8_t sectors0,
				   uint8_t rate_0, uint8_t rate_any,
				   uint32_t *tot_sectors, int keepBoot)
{
	int nb_renum;
	int sector2;
	int sum;

	set_word(boot->boot.nsect, dev->sectors);
	set_word(boot->boot.nheads, dev->heads);

#ifdef HAVE_ASSERT_H
	assert(*tot_sectors != 0);
#endif

	if (*tot_sectors <= UINT16_MAX && dev->hidden <= UINT16_MAX){
		set_word(boot->boot.psect, (uint16_t) *tot_sectors);
		set_dword(boot->boot.bigsect, 0);
		set_word(boot->boot.nhs, (uint16_t) dev->hidden);
	} else if(*tot_sectors <= UINT32_MAX){
		set_word(boot->boot.psect, 0);
		set_dword(boot->boot.bigsect, (uint32_t) *tot_sectors);
		set_dword(boot->boot.nhs, dev->hidden);
	} else {
		fprintf(stderr, "Too many sectors %u\n", *tot_sectors);
		exit(1);
	}

	if (dev->use_2m & 0x7f){
		uint16_t bootOffset;
		uint8_t j;
		uint8_t size2;
		uint16_t i;
		strncpy(boot->boot.banner, "2M-STV04", 8);
		boot->boot.ext.old.res_2m = 0;
		boot->boot.ext.old.fmt_2mf = 6;
		if ( dev->sectors % ( ((1 << dev->ssize) + 3) >> 2 ))
			boot->boot.ext.old.wt = 1;
		else
			boot->boot.ext.old.wt = 0;
		boot->boot.ext.old.rate_0= rate_0;
		boot->boot.ext.old.rate_any= rate_any;
		if (boot->boot.ext.old.rate_any== 2 )
			boot->boot.ext.old.rate_any= 1;
		i=76;

		/* Infp0 */
		set_word(boot->boot.ext.old.Infp0, i);
		boot->bytes[i++] = sectors0;
		boot->bytes[i++] = 108;
		for(j=1; j<= sectors0; j++)
			boot->bytes[i++] = j;

		set_word(boot->boot.ext.old.InfpX, i);

		boot->bytes[i++] = 64;
		boot->bytes[i++] = 3;
		nb_renum = i++;
		sector2 = dev->sectors;
		size2 = dev->ssize;
		j=1;
		while( sector2 ){
			while ( sector2 < (1 << size2) >> 2 )
				size2--;
			boot->bytes[i++] = 128 + j;
			boot->bytes[i++] = j++;
			boot->bytes[i++] = size2;
			sector2 -= (1 << size2) >> 2;
		}
		boot->bytes[nb_renum] = (uint8_t)(( i - nb_renum - 1 )/3);

		set_word(boot->boot.ext.old.InfTm, i);

		sector2 = dev->sectors;
		size2= dev->ssize;
		while(sector2){
			while ( sector2 < 1 << ( size2 - 2) )
				size2--;
			boot->bytes[i++] = size2;
			sector2 -= 1 << (size2 - 2 );
		}

		set_word(boot->boot.ext.old.BootP,i);
		bootOffset = i;

		/* checksum */
		for (sum=0, j=64; j<i; j++)
			sum += boot->bytes[j];/* checksum */
		boot->boot.ext.old.CheckSum=(unsigned char)-sum;
		return bootOffset;
	} else {
		if(!keepBoot) {
			boot->boot.jump[0] = 0xeb;
			boot->boot.jump[1] = 0;
			boot->boot.jump[2] = 0x90;
			strncpy(boot->boot.banner, mformat_banner, 8);
			/* It looks like some versions of DOS are
			 * rather picky about this, and assume default
			 * parameters without this, ignoring any
			 * indication about cluster size et al. */
		}
		return 0;
	}
}

static __inline__ void format_root(Fs_t *Fs, char *label, union bootsector *boot)
{
	Stream_t *RootDir;
	char *buf;
	unsigned int i;
	struct ClashHandling_t ch;
	unsigned int dirlen;

	init_clash_handling(&ch);
	ch.name_converter = label_name_uc;
	ch.ignore_entry = -2;

	buf = safe_malloc(Fs->sector_size);
	RootDir = OpenRoot((Stream_t *)Fs);
	if(!RootDir){
		fprintf(stderr,"Could not open root directory\n");
		exit(1);
	}

	memset(buf, '\0', Fs->sector_size);

	if(Fs->fat_bits == 32) {
		/* on a FAT32 system, we only write one sector,
		 * as the directory can be extended at will...*/
		dirlen = Fs->cluster_size;
		fatAllocate(Fs, Fs->rootCluster, Fs->end_fat);
	} else
		dirlen = Fs->dir_len;
	for (i = 0; i < dirlen; i++)
		WRITES(RootDir, buf, sectorsToBytes((Stream_t*)Fs, i),
			   Fs->sector_size);

	ch.ignore_entry = 1;
	if(label[0])
		mwrite_one(RootDir,label, 0, labelit, NULL,&ch);

	FREE(&RootDir);
	if(Fs->fat_bits == 32)
		set_word(boot->boot.dirents, 0);
	else
		set_word(boot->boot.dirents,
			 (uint16_t) (Fs->dir_len * (Fs->sector_size / 32)));
	free(buf);
}

/*
 * Calculate length of one FAT, in sectors, given the number of total sectors
 * Returns
 *  0: if a length was successfully calculated. (in that case, it is filled
 *  into Fs->fat_len)
 *  1: if the specified number of FAT bits cannot accomodate that many
 *  sectors => caller should raise FAT bits
 *  -1: if the specified number of FAT bits cannot accomodate so little
 *  sectors => caller should lower FAT bits again
 */
static int calc_fat_len(Fs_t *Fs, uint32_t tot_sectors)
{
	uint32_t rem_sect;
	uint32_t numerator;
	uint32_t denominator;
	uint32_t corr=0; /* correct numeric overflow */
	unsigned int fat_nybbles;

#ifdef HAVE_ASSERT_H
	assert(Fs->fat_bits != 0);
#endif

#ifdef DEBUG
	fprintf(stderr, "Fat start=%d\n", Fs->fat_start);
	fprintf(stderr, "tot_sectors=%lu\n", tot_sectors);
	fprintf(stderr, "dir_len=%d\n", Fs->dir_len);
#endif
	Fs->fat_len = 0;
	rem_sect = tot_sectors - calc_clus_start(Fs);
	
	/* Cheat a little bit to address the _really_ common case of
	   odd number of remaining sectors while both nfat and cluster size
	   are even... */
	if(rem_sect         % 2 == 1 &&
	   Fs->num_fat      % 2 == 0 &&
	   Fs->cluster_size % 2 == 0)
		rem_sect--;

#ifdef DEBUG
	fprintf(stderr, "Rem sect=%lu\n", rem_sect);
#endif

	/* See fat_size_calculation.tex or
	   (https://www.gnu.org/gnu/mtools/manual/fat_size_calculation.pdf)
	   for an explantation about why the stuff below works...
	*/

	fat_nybbles = Fs->fat_bits / 4;
	numerator   = rem_sect+2*Fs->cluster_size;
	/* Might overflow, but will be cancelled out below. As the
	   operation is unsigned, a posteriori fixup is allowable, as
	   wrap-around is part of the spec. For *signed* quantities,
	   this hack would be incorrect, as it would be "undefined
	   behavior" */

	/* Initial denominator is nybbles consumed by one cluster, both in
	 * FAT and in cluster space */
	denominator =
	  Fs->cluster_size * Fs->sector_size * 2 +
	  Fs->num_fat * fat_nybbles;

	if(fat_nybbles == 3) {
		/* We need to do this test here, or multiplying rem_sect with 
		 * fat_nybbles might overflow */
		if(rem_sect > 256 * FAT12)
			return 1;
		numerator *= fat_nybbles;
	} else
		/* Avoid numerical overflows, divide the denominator
		 * rather than multiplying the numerator */
		denominator = denominator / fat_nybbles;

	/* Substract denominator from numerator to "cancel out" an
	   unsigned integer overflow which might have happened with
	   total number of sectors very near maximum (2^32-1) and huge
	   cluster size. This substraction removes 1 from the result
	   of the following division, so we will add 1 again after the
	   division. However, we only do this if (original) numerator
	   is bigger than denominator though, as otherwise we risk the
	   inverse problem of going below 0 on small disks */
	if(rem_sect > denominator) {
		numerator -=  denominator;
		corr++;
	}
	
#ifdef DEBUG
	fprintf(stderr, "Numerator=%lu denominator=%lu\n",
		numerator, denominator);
#endif

	Fs->fat_len = (numerator-1)/denominator+1+corr;
	return 0;
}

/*
 * Given fat bits and cluster size, compute other FAT parameters
 * Return:
 *  0: if everything fits
 *  1: if there would be too many sectors for given cluster size and FAT bits. 
 *     Caller should raise cluster size or fat bits.
 *  2: if there would be too many clusters for given fat_len. Caller should 
 *     raise cluster size, but not fat_bits, as this would actually make this
 *     situation worse.
 * -1: if there would be too few sectors for given cluster size and FAT bits
 *     caller should lower fat_bits
 */
static int calc_other_fs_params(Fs_t *Fs,
				uint32_t tot_sectors,
				bool allowDirLenChange,
				bool allowPadFat)
{
	unsigned int provisional_fat_bits;
	uint32_t maxClus;
	uint32_t minClus;

	switch(Fs->fat_bits) {
	case 12:
		minClus = 1;
		maxClus = FAT12;
		break;
	case 16:
		minClus = 4096;
		maxClus = FAT16;
		break;
	case 32:
		minClus = FAT16;
		maxClus = FAT32;
		break;
	default:
#ifdef HAVE_ASSERT_H
		assert(false && "Bad number of FAT bits");
#endif
	}

	while(true) {
		uint32_t bwaste; /* How many sectors we need to "waste" */
		uint16_t waste;
		uint16_t dir_grow=0;
		uint16_t dir_shrink;
		uint16_t fat_grow;

		calc_num_clus(Fs, tot_sectors);
		/* If fat_len has been explicitly specified by caller, make sure
		 * that number of cluster fit within this fat_len */
		if(Fs->num_clus >
		   (Fs->fat_len * Fs->sector_size * 2 / (Fs->fat_bits/4))-2) {
			return 2; /* Caller should should pick a
				   * bigger cluster size, but not a
				   * larger FAT */
		}

		if(Fs->num_clus < minClus)
			return -1; /* Not enough clusters => caller
				    * should shrink FAT bits again */

		if(Fs->num_clus < maxClus)
			break; /* less than maximum => ok */
		
		if(!allowPadFat)
			/* No "padding" allowed => return to allow caller
			 * to chose different set of parameters
			 */
			return 1;

		/* As the number of clusters is specified nowhere in
		 * the boot sector, it will be calculated by removing
		 * everything else from total number of sectors, and
		 * dividing by sectors per cluster. This means that if
		 * we reduced the number of sectors available for cluster by 
		 * increasing anything else, we will be able to get under the
		 * maximum.
		 * We first try to augment the root directory's size, and
		 * then the FAT's size itself.
		 */
		bwaste = tot_sectors - Fs->clus_start -
			maxClus * Fs->cluster_size + 1;
#ifdef HAVE_ASSERT_H
		assert(bwaste <= UINT16_MAX);
#endif
		waste = (uint16_t) bwaste;

		if(allowDirLenChange) {
			dir_grow = 32 - Fs->dir_len;
			if(dir_grow > waste)
				dir_grow = waste;
			waste -= dir_grow;
		}
		fat_grow = (waste + Fs->num_fat - 1) / Fs->num_fat;
		Fs->fat_len += fat_grow;
		waste -= fat_grow * Fs->num_fat;
			
		/* Shrink directory again, but at most as by as much
		 * as we grew it earlyer */
		dir_shrink = waste - fat_grow * Fs->num_fat;
		if(dir_shrink > dir_grow)
			dir_shrink = dir_grow;
		dir_grow -= dir_shrink;
		Fs->dir_len += dir_grow;
	}

#ifdef DEBUG
	fprintf(stderr, "Num_clus=%d fat_len=%d nybbles=%d\n",
		Fs->num_clus, Fs->fat_len, fat_nybbles);
#endif

#ifdef HAVE_ASSERT_H
	/* number of clusters must be within allowable range for fat bits */
	assert(Fs->num_clus >= minClus);
	assert(Fs->num_clus < maxClus);

	/* if FAT bits is 32, dir_len must be zero, otherwise it must be
	 * non-zero */
	assert(Fs->fat_bits == 32 ? (Fs->dir_len == 0) : (Fs->dir_len != 0));

	/* Clusters must fill disk up entirely, except for small amount of
	 * slack smaller than one sector */
	assert(tot_sectors >=
	       Fs->clus_start + Fs->num_clus * Fs->cluster_size);
	assert(tot_sectors <=
	       Fs->clus_start + Fs->num_clus * Fs->cluster_size + 
	       Fs->cluster_size - 1);

	/* Fat must be big enough for all clusters */
	assert( ((Fs->num_clus+2) * Fs->fat_bits / 4) <=
		(Fs->fat_len*Fs->sector_size*2));
#endif
	provisional_fat_bits = Fs->fat_bits;
	set_fat(Fs);
#ifdef HAVE_ASSERT_H
	assert(provisional_fat_bits == Fs->fat_bits);
#endif
	if(Fs->fat_bits == 32) {
		Fs->primaryFat = 0;
		Fs->writeAllFats = 1;
		if(!Fs->backupBoot) {
			if(Fs->fat_start <= 6)
				Fs->backupBoot = Fs->fat_start - 1;
			else
				Fs->backupBoot=6;
		}
	
		if(Fs->fat_start < 3) {
			fprintf(stderr,
				"For FAT 32, reserved sectors need to be at least 3\n");
			exit(1);
		}

		if(Fs->fat_start <= Fs->backupBoot) {
			fprintf(stderr,
				"Reserved sectors (%d) must be more than backupBoot (%d)\n", Fs->fat_start, Fs->backupBoot);
			Fs->backupBoot = 0;
		}
	}
	return 0;
}


static unsigned char bootprog[]=
{0xfa, 0x31, 0xc0, 0x8e, 0xd8, 0x8e, 0xc0, 0xfc, 0xb9, 0x00, 0x01,
 0xbe, 0x00, 0x7c, 0xbf, 0x00, 0x80, 0xf3, 0xa5, 0xea, 0x00, 0x00,
 0x00, 0x08, 0xb8, 0x01, 0x02, 0xbb, 0x00, 0x7c, 0xba, 0x80, 0x00,
 0xb9, 0x01, 0x00, 0xcd, 0x13, 0x72, 0x05, 0xea, 0x00, 0x7c, 0x00,
 0x00, 0xcd, 0x19};

static __inline__ void inst_boot_prg(union bootsector *boot, uint16_t offset)
{
	memcpy(boot->bytes + offset, bootprog, sizeof(bootprog));
	if(offset - 2 < 0x80) {
	  /* short jump */
	  boot->boot.jump[0] = 0xeb;
	  boot->boot.jump[1] = (uint8_t) (offset -2);
	  boot->boot.jump[2] = 0x90;
	} else {
	  /* long jump, if offset is too large */
	  boot->boot.jump[0] = 0xe9;
	  boot->boot.jump[1] = (uint8_t) (offset - 3);
	  boot->boot.jump[2] = (uint8_t) ( (offset - 3) >> 8);
	}
	set_word(boot->boot.jump + offset + 20, offset + 24);
}

static int old_dos_size_to_geom(size_t size,
				unsigned int *cyls,
				unsigned short *heads,
				unsigned short *sects)
{
	struct OldDos_t *params = getOldDosBySize(size);
	if(params != NULL) {
		*cyls = params->tracks;
		*heads = params->heads;
		*sects = params->sectors;
		return 0;
	} else
		return 1;
}


void calc_fs_parameters(struct device *dev, bool fat32,
			uint32_t tot_sectors,
			struct Fs_t *Fs, uint8_t *descr)
{
	bool may_change_boot_size = (Fs->fat_start == 0);
	bool may_change_fat_bits = (dev->fat_bits == 0) && !fat32;
	bool may_change_cluster_size = (Fs->cluster_size == 0);
	bool may_change_root_size = (Fs->dir_len == 0);
	bool may_change_fat_len = (Fs->fat_len == 0);
	bool may_pad = false;
	uint16_t saved_dir_len;

	struct OldDos_t *params=NULL;
	Fs->infoSectorLoc = 0;
	if( (may_change_fat_bits || abs(dev->fat_bits) == 12) &&
	    (may_change_boot_size || Fs->fat_start == 1) )
		params = getOldDosByParams(dev->tracks,dev->heads,dev->sectors,
					   Fs->dir_len, Fs->cluster_size);
	if(params != NULL) {
		*descr = params->media;
		Fs->fat_start = 1;
		Fs->cluster_size = params->cluster_size;
		Fs->dir_len = params->dir_len;
		Fs->fat_len = params->fat_len;
		Fs->fat_bits = 12;
		calc_other_fs_params(Fs, tot_sectors, false, false);
		return;
	}

	/* a format described by BPB */
	if(dev->hidden || tot_sectors % (dev->sectors * dev->heads))
		*descr = 0xf8;
	else
		*descr = 0xf0;

	Fs->fat_bits = abs(dev->fat_bits);
	if(Fs->fat_bits == 0) 
		/* If fat_bits not specified by device, start with a 12-bit
		 * FAT, unless 32 bit specified on command line */
		Fs->fat_bits = fat32 ? 32 : 12;
	if(!Fs->cluster_size) {
		if(tot_sectors < 2400 && dev->heads == 2)
			/* double sided double density floppies */
			Fs->cluster_size = 2;
		else if(may_change_fat_len && Fs->fat_bits == 32)
			/* FAT32 => start with 8 */
			Fs->cluster_size = 8;
		else
			/* In all other cases, start with 1 */
			Fs->cluster_size = 1;
	}

	if(!Fs->dir_len) {
		if(tot_sectors < 1200) {
			/* Double density floppies */
			if (dev->heads == 1)
				Fs->dir_len = 4;
			else
				Fs->dir_len = 7;
		} else if(tot_sectors <= 3840)
			/* High density floppies */
			Fs->dir_len = 14;
		else if(tot_sectors <= 7680)
			/* extra density floppies */
			Fs->dir_len = 15;
		else
			Fs->dir_len = 32;
	}
	saved_dir_len = Fs->dir_len;

	while(true) {
		int fit=0;

		if(may_change_boot_size) {
			if(Fs->fat_bits == 32)
				Fs->fat_start = 32;
			else
				Fs->fat_start = 1;
		}

		if(Fs->fat_bits == 32)
			Fs->dir_len = 0;
		else if(Fs->dir_len == 0)
			Fs->dir_len = saved_dir_len;

		if(getenv("MTOOLS_DEBUG_FAT")) {
			fprintf(stderr, "FAT=%d Cluster=%d%s\n",
				Fs->fat_bits, Fs->cluster_size,
				may_pad ? " may_pad" : "");
		}
		
		if(may_change_fat_len)
			fit=calc_fat_len(Fs, tot_sectors);
		if(fit == 0)
			fit=calc_other_fs_params(Fs, tot_sectors,
						 may_change_root_size,
						 may_pad);
		if(getenv("MTOOLS_DEBUG_FAT")) {
			fprintf(stderr, " fit=%d\n", fit);
		}
		if(fit == 0)
			break;
#ifdef HAVE_ASSERT_H
		assert(!may_pad);
		assert(fit != 2 || !may_change_fat_len);
#endif
		if(fit < 0) {
			if(may_change_cluster_size &&
			   may_change_fat_len &&
			   Fs->cluster_size > 1) {
				Fs->cluster_size = Fs->cluster_size / 2;
				continue;
			}

			/* Somehow we ended up with too few sectors
			 * for FAT size. This can only happen if
			 * cluster size is not adjustable, and if we
			 * had *barely* more clusters than allowed by
			 * previous fat bits. After raising fat bits,
			 * fat_len grew larger (due to each individual
			 * FAT entry now being larger), pushing the
			 * number of clusters *below* new limit.  =>
			 * lower fat bits again */
			if(!may_change_fat_bits || fat32) {
				fprintf(stderr,
					"Too few clusters for %d bit fat\n",
					Fs->fat_bits);
			}
			switch(Fs->fat_bits) {
			case 12:
				/* This should not happen */
#ifdef HAVE_ASSERT_H
				assert(false && "Too few clusters for FAT12");
#endif
				break;
			case 16:
				Fs->fat_bits=12;
				break;
			case 32:
				Fs->fat_bits=16;
				break;
			}
			may_pad=true;
			continue;
		}

		if(fit == 1 && may_change_fat_bits) {
			/* If cluster_size reached
			 * "maximum" for fat_bits,
			 * switch over to next
			 */
			if(Fs->fat_bits == 12 &&
			   (!may_change_cluster_size ||
			    Fs->cluster_size >= 8)) {
				Fs->fat_bits = 16;
				if(may_change_cluster_size)
					Fs->cluster_size = 1;
				continue;
			}

			if(Fs->fat_bits == 16 &&
			   (!may_change_cluster_size ||
			    Fs->cluster_size >= 64)) {
				Fs->fat_bits = 32;
				if(may_change_cluster_size)
					Fs->cluster_size =
						may_change_fat_len ? 8 : 1;
				continue;
			}
		}

		if(may_change_cluster_size && Fs->cluster_size < 128) {
			/* Double cluster size, and try again */
			Fs->cluster_size = 2 * Fs->cluster_size;
			continue;
		}

		/* Still too many clusters */
		if(fit == 2) {
			fprintf(stderr, "Too many clusters for fat length %d\n",
				Fs->fat_len);
		} else {
			fprintf(stderr, "Too many clusters for %d bit FAT\n",
				Fs->fat_bits);
		}
		exit(1);
	}

	if(getenv("MTOOLS_DEBUG_FAT") || getenv("MTOOLS_DEBUG_FAT_SUMMARY")) {
		fprintf(stderr,
			" FAT%d Cluster_size=%d %d clusters FAT_LEN=%d\n",
			Fs->fat_bits,
			Fs->cluster_size,
			Fs->num_clus,
			Fs->fat_len);
	}
}

void initFsForFormat(Fs_t *Fs)
{
	memset(Fs, 0, sizeof(*Fs));
	
	Fs->Class = &FsClass;
	Fs->refs = 1;

	Fs->cluster_size = 0;
	Fs->dir_len = 0;
	Fs->fat_len = 0;
	Fs->num_fat = 2;
	Fs->backupBoot = 0;
}

void setFsSectorSize(Fs_t *Fs, struct device *dev, uint16_t msize) {
	unsigned int j;
	Fs->sector_size = 512;
	if( !(dev->use_2m & 0x7f)) {
		Fs->sector_size = (uint16_t) (128u << (dev->ssize & 0x7f));
	}

	SET_INT(Fs->sector_size, msize);
	for(j = 0; j < 31; j++) {
		if (Fs->sector_size == (unsigned int) (1 << j)) {
			Fs->sectorShift = j;
			break;
		}
	}
	Fs->sectorMask = Fs->sector_size - 1;
}

static void usage(int ret) NORETURN;
static void usage(int ret)
{
	fprintf(stderr,
		"Mtools version %s, dated %s\n", mversion, mdate);
	fprintf(stderr,
		"Usage: %s [-V] [-t tracks] [-h heads] [-n sectors] "
		"[-v label] [-1] [-4] [-8] [-f size] "
		"[-N serialnumber] "
		"[-k] [-B bootsector] [-r root_dir_len] [-L fat_len] "
		"[-F] [-I fsVersion] [-C] [-c cluster_size] "
		"[-H hidden_sectors] "
#ifdef USE_XDF
		"[-X] "
#endif
		"[-S hardsectorsize] [-M softsectorsize] [-3] "
		"[-2 track0sectors] [-0 rate0] [-A rateany] [-a]"
		"device\n", progname);
	exit(ret);
}

void mformat(int argc, char **argv, int dummy UNUSEDP) NORETURN;
void mformat(int argc, char **argv, int dummy UNUSEDP)
{
	int r; /* generic return value */
	Fs_t *Fs;
	unsigned int hs;
	int hs_set;
	unsigned int arguse_2m = 0;
	uint8_t sectors0=18; /* number of sectors on track 0 */
	int create = 0;
	uint8_t rate_0, rate_any;
	int mangled;
	uint8_t argssize=0; /* sector size */
	uint16_t msize=0;
	int fat32 = 0;
	struct label_blk_t *labelBlock;
	size_t bootOffset;

#ifdef USE_XDF
	unsigned int i;
	int format_xdf = 0;
	struct xdf_info info;
#endif
	union bootsector boot;
	char *bootSector=0;
	int c;
	int keepBoot = 0;
	struct device used_dev;
	unsigned int argtracks;
	uint16_t argheads, argsectors;
	uint32_t tot_sectors=0;
	size_t blocksize;

	char drive, name[EXPAND_BUF];

	char label[VBUFSIZE];

	dos_name_t shortlabel;
	struct device *dev;
	char errmsg[2100];

	uint32_t serial;
 	int serial_set;
	uint16_t fsVersion;
	uint8_t mediaDesc=0;
	bool haveMediaDesc=false;
	
	mt_size_t maxSize;

	int Atari = 0; /* should we add an Atari-style serial number ? */

	char *endptr;

	hs = hs_set = 0;
	argtracks = 0;
	argheads = 0;
	argsectors = 0;
	arguse_2m = 0;
	argssize = 0x2;
	label[0] = '\0';
	serial_set = 0;
	serial = 0;
	fsVersion = 0;

	Fs = New(Fs_t);
	if (!Fs) {
		fprintf(stderr, "Out of memory\n");
		exit(1);
	}
	initFsForFormat(Fs);
	if(getenv("MTOOLS_DIR_LEN")) {
		Fs->dir_len = atou16(getenv("MTOOLS_DIR_LEN"));
	  if(Fs->dir_len <= 0)
	    Fs->dir_len=0;
	}
	if(getenv("MTOOLS_NFATS")) {
		Fs->num_fat = atou8(getenv("MTOOLS_NFATS"));
	  if(Fs->num_fat <= 0)
	    Fs->num_fat=2;
	}
	rate_0 = mtools_rate_0;
	rate_any = mtools_rate_any;

	/* get command line options */
	if(helpFlag(argc, argv))
		usage(0);
	while ((c = getopt(argc,argv,
			   "i:148f:t:n:v:qub"
			   "kK:R:B:r:L:I:FCc:Xh:s:T:l:N:H:M:S:2:30:Aad:m:"))!= EOF) {
		errno = 0;
		endptr = NULL;
		switch (c) {
			case 'i':
				set_cmd_line_image(optarg);
				break;

			/* standard DOS flags */
			case '1':
				argheads = 1;
				break;
			case '4':
				argsectors = 9;
				argtracks = 40;
				break;
			case '8':
				argsectors = 8;
				argtracks = 40;
				break;
			case 'f':
				r=old_dos_size_to_geom(atoul(optarg),
						       &argtracks, &argheads,
						       &argsectors);
				if(r) {
					fprintf(stderr,
						"Bad size %s\n", optarg);
					exit(1);
				}
				break;
			case 't':
				argtracks = atou16(optarg);
				break;

			case 'T':
				tot_sectors = atoui(optarg);
				break;

			case 'n': /*non-standard*/
			case 's':
				argsectors = atou16(optarg);
				break;

			case 'l': /* non-standard */
			case 'v':
				strncpy(label, optarg, VBUFSIZE-1);
				label[VBUFSIZE-1] = '\0';
				break;

			/* flags supported by Dos but not mtools */
			case 'q':
			case 'u':
			case 'b':
			/*case 's': leave this for compatibility */
				fprintf(stderr,
					"Flag %c not supported by mtools\n",c);
				exit(1);



			/* flags added by mtools */
			case 'F':
				fat32 = 1;
				break;


			case 'S':
				argssize = atou8(optarg) | 0x80;
				if(argssize < 0x80)
					usage(1);
				if(argssize >= 0x87) {
					fprintf(stderr, "argssize must be less than 6\n");
					usage(1);
				}
				break;

#ifdef USE_XDF
			case 'X':
				format_xdf = 1;
				break;
#endif

			case '2':
				arguse_2m = 0xff;
				sectors0 = atou8(optarg);
				break;
			case '3':
				arguse_2m = 0x80;
				break;

			case '0': /* rate on track 0 */
				rate_0 = atou8(optarg);
				break;
			case 'A': /* rate on other tracks */
				rate_any = atou8(optarg);
				break;

			case 'M':
				msize = atou16(optarg);
				if(msize != 512 &&
				   msize != 1024 &&
				   msize != 2048 &&
				   msize != 4096) {
				  fprintf(stderr, "Only sector sizes of 512, 1024, 2048 or 4096 bytes are allowed\n");
				  usage(1);
				}
				break;

			case 'N':
 				serial = strtou32(optarg,&endptr,16);
 				serial_set = 1;
 				break;
			case 'a': /* Atari style serial number */
				Atari = 1;
				break;

			case 'C':
				create = O_CREAT | O_TRUNC;
				break;

			case 'H':
				hs = atoui(optarg);
				hs_set = 1;
				break;

			case 'I':
				fsVersion = strtou16(optarg,&endptr,0);
				break;

			case 'c':
				Fs->cluster_size = atoui(optarg);
				break;

			case 'r':
				Fs->dir_len = strtou16(optarg,&endptr,0);
				break;
			case 'L':
				Fs->fat_len = strtoui(optarg,&endptr,0);
				break;


			case 'B':
				bootSector = optarg;
				break;
			case 'k':
				keepBoot = 1;
				break;
			case 'K':
				Fs->backupBoot = atou16(optarg);
				if(Fs->backupBoot < 2) {
				  fprintf(stderr, "Backupboot must be greater than 2\n");
				  exit(1);
				}
				break;
			case 'R':
				Fs->fat_start = atou8(optarg);
				break;
			case 'h':
				argheads = atou16(optarg);
				break;
			case 'd':
				Fs->num_fat = atou8(optarg);
				break;
			case 'm':
				mediaDesc = strtou8(optarg,&endptr,0);
				if(*endptr)
					mediaDesc = strtou8(optarg,&endptr,16);
				if(optarg == endptr || *endptr) {
				  fprintf(stderr, "Bad mediadesc %s\n", optarg);
				  exit(1);
				}
				haveMediaDesc=true;
				break;
			default:
				usage(1);
		}
		check_number_parse_errno((char)c, optarg, endptr);
	}

	if (argc - optind > 1)
		usage(1);
	if(argc - optind == 1) {
	    if(!argv[optind][0] || argv[optind][1] != ':')
		usage(1);
	    drive = ch_toupper(argv[argc -1][0]);
	} else {
	    drive = get_default_drive();
	    if(drive != ':') {
	      /* Use default drive only if it is ":" (image file), as else
		 it would be too dangerous... */
	      fprintf(stderr, "Drive letter missing\n");
	      exit(1);
	    }
	}

	if(argtracks && tot_sectors) {
		fprintf(stderr, "Only one of -t or -T may be specified\n");
		usage(1);
	}

#ifdef USE_XDF
	if(create && format_xdf) {
		fprintf(stderr,"Create and XDF can't be used together\n");
		exit(1);
	}
#endif

	/* check out a drive whose letter and parameters match */
	sprintf(errmsg, "Drive '%c:' not supported", drive);
	Fs->Direct = NULL;
	blocksize = 0;
	for(dev=devices;dev->drive;dev++) {
		FREE(&(Fs->Direct));
		/* drive letter */
		if (dev->drive != drive)
			continue;
		used_dev = *dev;

		SET_INT(used_dev.tracks, argtracks);
		SET_INT(used_dev.heads, argheads);
		SET_INT(used_dev.sectors, argsectors);
		SET_INT(used_dev.use_2m, arguse_2m);
		SET_INT(used_dev.ssize, argssize);
		if(hs_set)
			used_dev.hidden = hs;

		expand(dev->name, name);
#ifdef USING_NEW_VOLD
		strcpy(name, getVoldName(dev, name));
#endif

#ifdef USE_XDF
		if(format_xdf)
			used_dev.misc_flags |= USE_XDF_FLAG;
#endif
		if(tot_sectors)
			used_dev.tot_sectors = tot_sectors;
		info.FatSize=0;
		Fs->Direct = OpenImage(&used_dev, dev, name,
				      O_RDWR|create, errmsg,
				      ALWAYS_GET_GEOMETRY,
				      O_RDWR,
				      &maxSize, NULL,
#ifdef USE_XDF
				      &info
#else
				      NULL
#endif
				      );

#ifdef USE_XDF
		if(Fs->Direct && info.FatSize) {
			if(!Fs->fat_len)
				Fs->fat_len = info.FatSize;
			if(!Fs->dir_len)
				Fs->dir_len = info.RootDirSize;
		}
#endif

		if (!Fs->Direct)
			continue;

		if(!tot_sectors)
			tot_sectors = used_dev.tot_sectors;

		setFsSectorSize(Fs, &used_dev, msize);

		if(!used_dev.blocksize || used_dev.blocksize < Fs->sector_size)
			blocksize = Fs->sector_size;
		else
			blocksize = used_dev.blocksize;

		if(blocksize > MAX_SECTOR)
			blocksize = MAX_SECTOR;

		if((mt_size_t) tot_sectors * blocksize > maxSize) {
			snprintf(errmsg, sizeof(errmsg)-1,
				 "Requested size too large\n");
			FREE(&Fs->Direct);
			continue;
		}


		/* do a "test" read */
		if (!create &&
		    READS(Fs->Direct, &boot.characters, 0, Fs->sector_size) !=
		    (signed int) Fs->sector_size) {
#ifdef HAVE_SNPRINTF
			snprintf(errmsg, sizeof(errmsg)-1,
				 "Error reading from '%s', wrong parameters?",
				 name);
#else
			sprintf(errmsg,
				"Error reading from '%s', wrong parameters?",
				name);
#endif
			FREE(&Fs->Direct);
			continue;
		}
		break;
	}

	/* print error msg if needed */
	if ( dev->drive == 0 ){
		FREE(&Fs->Direct);
		fprintf(stderr,"%s: %s\n", argv[0],errmsg);
		exit(1);
	}

	/* calculate the total number of sectors */
	if(tot_sectors == 0 &&
	   used_dev.heads && used_dev.sectors && used_dev.tracks) {
		uint32_t sect_per_track = used_dev.heads*used_dev.sectors;
		mt_off_t rtot_sectors =
			used_dev.tracks*(mt_off_t)sect_per_track;
		if(rtot_sectors > used_dev.hidden%sect_per_track)
			rtot_sectors -= used_dev.hidden%sect_per_track;
		tot_sectors = mt_off_t_to_sectors(rtot_sectors);
	}

	if(tot_sectors == 0) {
		fprintf(stderr, "Number of sectors not known\n");
		exit(1);
	}

	/* create the image file if needed */
	if (create) {
		WRITES(Fs->Direct, &boot.characters,
		       sectorsToBytes((Stream_t*)Fs, tot_sectors-1),
		       Fs->sector_size);
	}

	/* the boot sector */
	if(bootSector) {
		int fd;
		ssize_t ret;
		
		fd = open(bootSector, O_RDONLY | O_BINARY | O_LARGEFILE);
		if(fd < 0) {
			perror("open boot sector");
			exit(1);
		}
		ret=read(fd, &boot.bytes, blocksize);
		if(ret < 0 || (size_t) ret < blocksize) {
			perror("short read on boot sector");
			exit(1);
		}
		keepBoot = 1;
		close(fd);
	}
	if(!keepBoot && !(used_dev.use_2m & 0x7f))
		memset(boot.characters, '\0', Fs->sector_size);

	Fs->Next = buf_init(Fs->Direct,
			    blocksize * used_dev.heads * used_dev.sectors,
			    blocksize * used_dev.heads * used_dev.sectors,
			    blocksize);
	Fs->Buffer = 0;

	boot.boot.nfat = Fs->num_fat;
	if(!keepBoot)
		set_word(&boot.bytes[510], 0xaa55);

	/* Initialize the remaining parameters */
	set_word(boot.boot.nsect, used_dev.sectors);
	set_word(boot.boot.nheads, used_dev.heads);

	calc_fs_parameters(&used_dev, fat32, tot_sectors, Fs, &boot.boot.descr);

	if(!keepBoot && !(used_dev.use_2m & 0x7f)) {
		if(!used_dev.partition) {
			/* install fake partition table pointing to itself */
			struct partition *partTable=(struct partition *)
				(&boot.bytes[0x1ae]);
			setBeginEnd(&partTable[1], 0,
				    used_dev.heads * used_dev.sectors *
				    used_dev.tracks,
				    (uint8_t) used_dev.heads,
				    (uint8_t) used_dev.sectors, 1, 0,
				    Fs->fat_bits);
		}
	}

	if(Fs->fat_bits == 32) {
		set_word(boot.boot.fatlen, 0);
		set_dword(boot.boot.ext.fat32.bigFat, Fs->fat_len);

		Fs->clus_start = Fs->num_fat * Fs->fat_len + Fs->fat_start;

		/* extension flags: mirror fats, and use #0 as primary */
		set_word(boot.boot.ext.fat32.extFlags,0);

		/* fs version.  What should go here? */
		set_word(boot.boot.ext.fat32.fsVersion,fsVersion);

		/* root directory */
		set_dword(boot.boot.ext.fat32.rootCluster, Fs->rootCluster = 2);

		/* info sector */
		set_word(boot.boot.ext.fat32.infoSector, Fs->infoSectorLoc = 1);
		Fs->infoSectorLoc = 1;

		/* no backup boot sector */
		set_word(boot.boot.ext.fat32.backupBoot, Fs->backupBoot);

		labelBlock = & boot.boot.ext.fat32.labelBlock;
	} else {
		set_word(boot.boot.fatlen, (uint16_t) Fs->fat_len);
		Fs->dir_start = Fs->num_fat * Fs->fat_len + Fs->fat_start;
		Fs->clus_start = Fs->dir_start + Fs->dir_len;
		labelBlock = & boot.boot.ext.old.labelBlock;
	}

	/* Set the codepage */
	Fs->cp = cp_open(used_dev.codepage);
	if(Fs->cp == NULL)
		exit(1);

	if (!keepBoot)
		/* only zero out physdrive if we don't have a template
		 * bootsector */
		labelBlock->physdrive = 0x00;
	labelBlock->reserved = 0;
	labelBlock->dos4 = 0x29;

	if (!serial_set || Atari)
		init_random();
	if (!serial_set)
		serial=(uint32_t) random();
	set_dword(labelBlock->serial, serial);
	label_name_pc(GET_DOSCONVERT((Stream_t *)Fs),
		      label[0] ? label : "NO NAME    ", 0,
		      &mangled, &shortlabel);
	strncpy(labelBlock->label, shortlabel.base, 8);
	strncpy(labelBlock->label+8, shortlabel.ext, 3);
	sprintf(labelBlock->fat_type, "FAT%2.2d  ", Fs->fat_bits);
	labelBlock->fat_type[7] = ' ';

	set_word(boot.boot.secsiz, Fs->sector_size);
	boot.boot.clsiz = (unsigned char) Fs->cluster_size;
	set_word(boot.boot.nrsvsect, Fs->fat_start);

	bootOffset = init_geometry_boot(&boot, &used_dev, sectors0,
					rate_0, rate_any,
					&tot_sectors, keepBoot);
	if(!bootOffset) {
		bootOffset = ptrdiff((char *) labelBlock, (char*)boot.bytes) +
			sizeof(struct label_blk_t);
	}
	if(Atari) {
		boot.boot.banner[4] = 0;
		boot.boot.banner[5] = (char) random();
		boot.boot.banner[6] = (char) random();
		boot.boot.banner[7] = (char) random();
	}

	if(!keepBoot && bootOffset <= UINT16_MAX)
		inst_boot_prg(&boot, (uint16_t)bootOffset);
	/* Mimic 3.8 behavior, else 2m disk do not work (???)
	 * luferbu@fluidsignal.com (Luis Bustamante), Fri, 14 Jun 2002
	 */
	if(used_dev.use_2m & 0x7f) {
	  boot.boot.jump[0] = 0xeb;
	  boot.boot.jump[1] = 0x80;
	  boot.boot.jump[2] = 0x90;
	}
	if(used_dev.use_2m & 0x7f)
		Fs->num_fat = 1;
	if(haveMediaDesc)
		boot.boot.descr=mediaDesc;
	Fs->lastFatSectorNr = 0;
	Fs->lastFatSectorData = 0;
	zero_fat(Fs, boot.boot.descr);
	Fs->freeSpace = Fs->num_clus;
	Fs->last = 2;

#ifdef USE_XDF
	if(format_xdf)
		for(i=0;
		    i < (info.BadSectors+Fs->cluster_size-1)/Fs->cluster_size;
		    i++)
			fatEncode(Fs, i+2, 0xfff7);
#endif

	format_root(Fs, label, &boot);
	if(WRITES((Stream_t *)Fs, boot.characters,
		  (mt_off_t) 0, Fs->sector_size) < 0) {
		fprintf(stderr, "Error writing boot sector\n");
		exit(1);
	}

	if(Fs->fat_bits == 32 && WORD_S(ext.fat32.backupBoot) != MAX16) {
		if(WRITES((Stream_t *)Fs, boot.characters,
			  sectorsToBytes((Stream_t*)Fs,
					 WORD_S(ext.fat32.backupBoot)),
			  Fs->sector_size) < 0) {
			fprintf(stderr, "Error writing backup boot sector\n");
			exit(1);
		}
	}

	FREE((Stream_t **)&Fs);
#ifdef USE_XDF
	if(format_xdf && isatty(0) && !getenv("MTOOLS_USE_XDF"))
		fprintf(stderr,
			"Note:\n"
			"Remember to set the \"MTOOLS_USE_XDF\" environmental\n"
			"variable before accessing this disk\n\n"
			"Bourne shell syntax (sh, ash, bash, ksh, zsh etc):\n"
			" export MTOOLS_USE_XDF=1\n\n"
			"C shell syntax (csh and tcsh):\n"
			" setenv MTOOLS_USE_XDF 1\n" );
#endif
	exit(0);
}
