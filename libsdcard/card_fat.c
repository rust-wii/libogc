#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <ctype.h>

#include "cache.h"
#include "mutex.h"

#include "card_cmn.h"
#include "card_buf.h"
#include "card_io.h"
#include "card_fat.h"

#define FAT_RETURN(x, y) \
                do { card_postFAT((x));  return (y); } while (0)

#define SET_FAT_TBL(drv_no, offset, value) \
                do { \
                        if((u32)(offset)<_fatTblIdxCnt[(drv_no)]) { \
                                _fat[(drv_no)][(u32)(offset)] = (value); \
                        } \
                } while (0)

#define GET_FAT_TBL(drv_no, offset) \
                (((u32)(offset)<_fatTblIdxCnt[(drv_no)]) \
                        ?_fat[(drv_no)][(u32)(offset)]:0)


static sd_info _sdInfo[MAX_DRIVE];

static mutex_t _fatLock[MAX_DRIVE];
static opendfile_list *_fatOpenedFile[MAX_DRIVE];
static u32 _fatFlag[MAX_DRIVE];
static u32 _fatTblIdxCnt[MAX_DRIVE];
static u32 _fat1StartSect[MAX_DRIVE];
static u32 _fat1Sectors[MAX_DRIVE];
static u32 _fatRootStartSect[MAX_DRIVE];
static u32 _fatRootSects[MAX_DRIVE];
static u32 _fatClusterStartSect[MAX_DRIVE];
static u32 _fatCacheSize[MAX_DRIVE] = {0,0};
static u8 _drvName[MAX_DRIVE][MAX_DRIVE_NAME_LEN+1];

static u16 *_fat[MAX_DRIVE] = {NULL,NULL};
static opendfile_list *_fatOpenedFile[MAX_DRIVE] = {NULL,NULL};

static void (*pfCallbackIN[MAX_DRIVE])(s32) = {NULL, NULL};
static void (*pfCallbackOUT[MAX_DRIVE])(s32) = {NULL, NULL};

s32 card_findEntryInDirectory(u32 drv_no,u32 find_mode,F_HANDLE h_parent,u32 var_par,u8* p_info,u32* p_cluster,u32* p_offset);

extern u32 card_convertStrToUni(u16 *dest,u8 *src,u32 len);
extern void card_uniToUpper(u16 *dest,u16 *src,u32 len);

static boolean card_extractLastName(const u8 *p_name,u8 *p_last_name)
{
	u32 len,i;

	len = strlen(p_name);
	for(i=len-2;i>=0;--i) {
		if(p_name[i]=='\\') break;
	}
	
	strcpy((char*)p_last_name,(char*)&p_name[i+1]);
	
	len = strlen(p_last_name);
	if(p_last_name[len-1]=='\\')
		p_last_name[len-1] = 0;

	return TRUE;
}

void card_initFATDefault()
{
	u8 name[10];
	u32 drv_no;

	printf("card_initFATDefault()\n");

	strcpy((char*)name,"dev");
	for(drv_no=0;drv_no<MAX_DRIVE;++drv_no) {
		_fatFlag[drv_no] = NOT_INITIALIZED;
		if(_fat[drv_no]) {
			free(_fat[drv_no]);
			_fat[drv_no] = NULL;
		}

		name[3] = (u8)drv_no + '0';
		name[4] = 0;
		strcpy((char*)_drvName[drv_no],(char*)name);

		LWP_MutexInit(&_fatLock[drv_no],FALSE);
	}
}

s32 card_initFAT(s32 drv_no)
{
	s32 ret;
	u8 *fat_buf;
	u32 offset;
	u32 fat_bytes,sector_plus,i,j;
	u32 pbr_offset,tot_clusters;

	_fatFlag[drv_no] = NOT_INITIALIZED;

	printf("card_initFAT(%d)\n",drv_no);
	
	fat_buf = card_allocBuffer();
	ret = card_readSector(drv_no,0,fat_buf,SECTOR_SIZE);
	if(ret!=0) {
		card_freeBuffer(fat_buf);
		return CARDIO_ERROR_INTERNAL;
	}

	pbr_offset = 0x1be;
	_sdInfo[drv_no].smbr.signature = ((u16)fat_buf[0x1fe]<<8)|fat_buf[0x1ff];
	_sdInfo[drv_no].smbr.partition_entries[0].def_boot = fat_buf[pbr_offset+0];
	_sdInfo[drv_no].smbr.partition_entries[0].start_cyl = ((u32)(fat_buf[pbr_offset+2]&0xc0)<<2)|fat_buf[pbr_offset+3];
	_sdInfo[drv_no].smbr.partition_entries[0].start_head = fat_buf[pbr_offset+1];
	_sdInfo[drv_no].smbr.partition_entries[0].start_sector = (fat_buf[pbr_offset + 2]&0x3f)-1;
	_sdInfo[drv_no].smbr.partition_entries[0].pt_type = fat_buf[pbr_offset+4];
	_sdInfo[drv_no].smbr.partition_entries[0].end_cyl = ((u32)(fat_buf[pbr_offset+6]&0xc0)<<2)|fat_buf[pbr_offset+7];
	_sdInfo[drv_no].smbr.partition_entries[0].end_head = fat_buf[pbr_offset+5];
	_sdInfo[drv_no].smbr.partition_entries[0].end_sector = (fat_buf[pbr_offset+6]&0x3f)-1;
	_sdInfo[drv_no].smbr.partition_entries[0].start_lba_sector = ((u32)fat_buf[pbr_offset+11]<<24)|((u32)fat_buf[pbr_offset+10]<<16)
																 |((u32)fat_buf[pbr_offset+9]<<8)|fat_buf[pbr_offset+8];
	_sdInfo[drv_no].smbr.partition_entries[0].total_avail_sectors = ((u32)fat_buf[pbr_offset+15]<<24)|((u32)fat_buf[pbr_offset+14]<<16)
                                                                    |((u32)fat_buf[pbr_offset+13]<<8)|fat_buf[pbr_offset+12];

	printf("mbr.signature = %04x\n",_sdInfo[drv_no].smbr.signature);
	printf("mbr.start_lba = %08x\n",_sdInfo[drv_no].smbr.partition_entries[0].start_lba_sector);

	if(_sdInfo[drv_no].smbr.signature!=0x55aa) return CARDIO_ERROR_INVALIDMBR;

	ret = card_readSector(drv_no,_sdInfo[drv_no].smbr.partition_entries[0].start_lba_sector,fat_buf,SECTOR_SIZE);
	if(ret!=0) {
		card_freeBuffer(fat_buf);
		return CARDIO_ERROR_INTERNAL;
	}

	memcpy(_sdInfo[drv_no].spbr.oem_name,&fat_buf[0x03],8);
	_sdInfo[drv_no].spbr.oem_name[8] = 0;
	_sdInfo[drv_no].spbr.drv_no = fat_buf[0x24];
    _sdInfo[drv_no].spbr.reserved = fat_buf[0x25];
    _sdInfo[drv_no].spbr.ext_boot_signature = fat_buf[0x26];
    _sdInfo[drv_no].spbr.vol_id = ((u32)fat_buf[0x2a]<<24)|((u32)fat_buf[0x29]<<16)|((u32)fat_buf[0x28]<<8)|fat_buf[0x27];
	memcpy(_sdInfo[drv_no].spbr.vol_label,&fat_buf[0x2b],11);

	_sdInfo[drv_no].spbr.vol_label[11] = 0;
	if(memcmp(&fat_buf[0x36],"FAT12   ",8)==0) _sdInfo[drv_no].spbr.file_sys_type = FS_FAT12;
	else if(memcmp(&fat_buf[0x36],"FAT16   ",8)==0) _sdInfo[drv_no].spbr.file_sys_type = FS_FAT16;
	else {
		_sdInfo[drv_no].spbr.file_sys_type = FS_UNKNOWN;
		card_freeBuffer(fat_buf);
		return CARDIO_ERROR_INVALIDPBR;
	}
	_sdInfo[drv_no].spbr.signature = ((u16)fat_buf[0x1fe]<<8)|fat_buf[0x1ff];
	_sdInfo[drv_no].spbr.sbpb.bytes_per_sect = ((u32)fat_buf[0xc]<<8)|fat_buf[0xb];
	_sdInfo[drv_no].spbr.sbpb.sects_per_cluster = fat_buf[0xd];
	_sdInfo[drv_no].spbr.sbpb.reserved_sects = ((u32)fat_buf[0xf]<<8)|fat_buf[0xe];
	_sdInfo[drv_no].spbr.sbpb.fat_num = fat_buf[0x10];
	_sdInfo[drv_no].spbr.sbpb.root_entry     = ((u32)fat_buf[0x12]<<8)|fat_buf[0x11];
	_sdInfo[drv_no].spbr.sbpb.total_sects = ((u32)fat_buf[0x14]<<8)|fat_buf[0x13];
	_sdInfo[drv_no].spbr.sbpb.fmt_type = fat_buf[0x15];
	_sdInfo[drv_no].spbr.sbpb.sects_in_fat = ((u32)fat_buf[0x17]<<8)|fat_buf[0x16];
	_sdInfo[drv_no].spbr.sbpb.sects_per_track = ((u32)fat_buf[0x19]<<8)|fat_buf[0x18];
	_sdInfo[drv_no].spbr.sbpb.head_num = ((u32)fat_buf[0x1b]<<8)|fat_buf[0x1a];
	_sdInfo[drv_no].spbr.sbpb.hidden_sects = ((u32)fat_buf[0x1f]<<24)|((u32)fat_buf[0x1e]<<16)|((u32)fat_buf[0x1d]<<8)|fat_buf[0x1c];
	_sdInfo[drv_no].spbr.sbpb.huge_sects = ((u32)fat_buf[0x23]<<24)|((u32)fat_buf[0x22]<<16)|((u32)fat_buf[0x21]<<8)|fat_buf[0x20];

	if(_sdInfo[drv_no].spbr.sbpb.total_sects==0) _sdInfo[drv_no].spbr.sbpb.total_sects = _sdInfo[drv_no].spbr.sbpb.huge_sects;

	printf("pbr.oem_name = %s\n",_sdInfo[drv_no].spbr.oem_name);
	printf("bpb.fmt_type = %02x\n",_sdInfo[drv_no].spbr.sbpb.fmt_type);
	printf("bpb.reserved_sects = %d\n",_sdInfo[drv_no].spbr.sbpb.reserved_sects);
	printf("bpb.bytes_per_sect = %d\n",_sdInfo[drv_no].spbr.sbpb.bytes_per_sect);
	printf("bpb.root_entry = %d\n",_sdInfo[drv_no].spbr.sbpb.root_entry);
	printf("bpb.sects_in_fat = %d\n",_sdInfo[drv_no].spbr.sbpb.sects_in_fat);
	printf("bpb.fat_num = %d\n",_sdInfo[drv_no].spbr.sbpb.fat_num);
	printf("bpb.sects_per_cluster = %d\n",_sdInfo[drv_no].spbr.sbpb.sects_per_cluster);
	printf("bpb.total_sects = %d\n",_sdInfo[drv_no].spbr.sbpb.total_sects);
	
	// spbr check recommended by SSFDC forum 
	if((_sdInfo[drv_no].spbr.signature!=0x55aa)
		|| (_sdInfo[drv_no].spbr.sbpb.bytes_per_sect!=512)
		|| (_sdInfo[drv_no].spbr.sbpb.fat_num<1 || _sdInfo[drv_no].spbr.sbpb.fat_num>16)
		|| (_sdInfo[drv_no].spbr.sbpb.root_entry==0)
		|| (_sdInfo[drv_no].spbr.sbpb.fmt_type!=0xf8)) 
	{
		card_freeBuffer(fat_buf);
		return CARDIO_ERROR_INVALIDPBR;
	}

	if(_sdInfo[drv_no].spbr.sbpb.fat_num>2) {
		card_freeBuffer(fat_buf);
		return CARDIO_ERROR_INVALIDPBR;
	}

	if(_fat[drv_no]) free(_fat[drv_no]);
	
	tot_clusters = _sdInfo[drv_no].spbr.sbpb.total_sects/_sdInfo[drv_no].spbr.sbpb.sects_per_cluster;

	printf("tot_clusters = %d\n",tot_clusters);
	
	_fatTblIdxCnt[drv_no] = tot_clusters;
	_fat[drv_no] = (u16*)malloc(_fatTblIdxCnt[drv_no]*sizeof(u16));
	if(!_fat[drv_no]) {
		card_freeBuffer(fat_buf);
		return CARDIO_ERROR_INTERNAL;
	}

	fat_bytes = tot_clusters*2;
	_fat1StartSect[drv_no] = _sdInfo[drv_no].smbr.partition_entries[0].start_lba_sector+_sdInfo[drv_no].spbr.sbpb.reserved_sects;
	_fat1Sectors[drv_no] = fat_bytes/_sdInfo[drv_no].spbr.sbpb.bytes_per_sect;		//holds the count of FAT sectors
	
	printf("_fat1StartSect[%d] = %d\n",drv_no,_fat1StartSect[drv_no]);
	printf("_fat1Sectors[%d] = %d\n",drv_no,_fat1Sectors[drv_no]);

	sector_plus = 0;
	for(i=0;i<_sdInfo[drv_no].spbr.sbpb.fat_num;++i) {
		ret = card_readSector(drv_no,_fat1StartSect[drv_no],fat_buf,SECTOR_SIZE);
		if(ret!=0) {
			card_freeBuffer(fat_buf);
			return CARDIO_ERROR_INTERNAL;
		}
		if(fat_buf[0]!=0xf8 || fat_buf[1]!=0xff || fat_buf[2]!=0xff) {
			sector_plus += _fat1Sectors[drv_no]*SECTOR_SIZE;
			continue;
		} else
			break;
	}
	if(i>=_sdInfo[drv_no].spbr.sbpb.fat_num) {
		card_freeBuffer(fat_buf);
		return CARDIO_ERROR_INVALIDFAT;
	}
	if(_sdInfo[drv_no].spbr.file_sys_type==FS_FAT12) {
		u32 remaind;
		u32 unread_cnt;
		
		offset = 0;
		remaind = 0;
		unread_cnt = 0;
		for(i=0;i<_fat1Sectors[drv_no];++i) {
			if(unread_cnt>0) {
				//ret = card_readSector()
			}
		}
		
	} else {
		offset = 0;
		for(i=0;i<_fat1Sectors[drv_no];++i) {
			ret = card_readSector(drv_no,_fat1StartSect[drv_no]+sector_plus+i,fat_buf,SECTOR_SIZE);
			if(ret!=0) {
				card_freeBuffer(fat_buf);
				return CARDIO_ERROR_INTERNAL;
			}

			for(j=0;j<=SECTOR_SIZE-2;j+=2) {
				SET_FAT_TBL(drv_no,offset,(((u32)fat_buf[j+1]<<8)|fat_buf[j]));
				++offset;

				if(offset>=3) {
					if(GET_FAT_TBL(drv_no,offset-1)>=0xfff0) {
						if(GET_FAT_TBL(drv_no,offset-1)>=0xfff8 && GET_FAT_TBL(drv_no,offset-1)<=0xffff)
							SET_FAT_TBL(drv_no,offset-1,LAST_CLUSTER);
						else if(GET_FAT_TBL(drv_no,offset-1)==0xfff7)
							SET_FAT_TBL(drv_no,offset-1,DEFECTIVE_CLUSTER);
					}
					if(offset>=tot_clusters+2) break;
				}
			}
		}
	}
	card_freeBuffer(fat_buf);

	_fatRootStartSect[drv_no] = _fat1StartSect[drv_no]+_fat1Sectors[drv_no]*_sdInfo[drv_no].spbr.sbpb.fat_num;
	_fatRootSects[drv_no] = (_sdInfo[drv_no].spbr.sbpb.root_entry*32+SECTOR_SIZE-1)/SECTOR_SIZE;
	_fatClusterStartSect[drv_no] = _fatRootStartSect[drv_no]+_fatRootSects[drv_no];

	printf("root_start = %d\n",_fatRootStartSect[drv_no]);
	printf("root_sects = %d\n",_fatRootSects[drv_no]);
	printf("cluster_start = %d\n",_fatClusterStartSect[drv_no]);

	_fatCacheSize[drv_no] = SECTOR_SIZE;

	_fatFlag[drv_no] = INITIALIZED;
	return CARDIO_ERROR_READY;	
}

s32 card_preFAT(s32 drv_no)
{
	s32 ret;

	LWP_MutexLock(&_fatLock[drv_no]);

	if(_fatFlag[drv_no]!=INITIALIZED) {
		ret = card_initFAT(drv_no);
		if(ret!=0) return ret;
	}
	return CARDIO_ERROR_READY;
}

s32 card_postFAT(s32 drv_no)
{
	LWP_MutexUnlock(&_fatLock[drv_no]);
	return CARDIO_ERROR_READY;
}

void card_insertedCB(s32 drv_no)
{
	if(pfCallbackIN[drv_no])
		pfCallbackIN[drv_no](drv_no);
}

void card_ejectedCB(s32 drv_no)
{
	if(pfCallbackOUT[drv_no])
		pfCallbackOUT[drv_no](drv_no);
}

u32 card_getDriveNo(const char *psz_name)
{
	u32 i,j,len;
	u8 drv_name[MAX_DRIVE_NAME_LEN+1];
	
	len = strlen(psz_name);
	for(i=0;i<len;++i) {
		if(psz_name[i]==':') break;
	}
	if(i<len && i<MAX_DRIVE_NAME_LEN) {
		memcpy(drv_name,psz_name,i);
		drv_name[i] = 0;
		for(j=0;j<MAX_DRIVE;++j) {
			if(strcmp((char*)drv_name,(char*)_drvName[j])==0) return j;
		}
	}
	return MAX_DRIVE;
}

s32 card_readCluster(s32 drv_no,u32 cluster_no,u32 offset,void *buf,u32 len)
{
	s32 ret;
	u32 start,end,i;
	u32 block_start_no;
	u32 block_end_no;
	u32 sects_per_block;
	u32 bytes_per_block;
	u32 top_size,bottom_size;
	s32 signed_cluster_no;
	u32 extra_sects;
	u8 *read_buf;

	read_buf = (u8*)buf;
	if(cluster_no==ROOT_HANDLE) {
		signed_cluster_no = 2-((_fatRootSects[drv_no]-1)/_sdInfo[drv_no].spbr.sbpb.sects_per_cluster+1);
		extra_sects = _fatRootSects[drv_no]%_sdInfo[drv_no].spbr.sbpb.sects_per_cluster;
		if(extra_sects) offset += (_sdInfo[drv_no].spbr.sbpb.sects_per_cluster-extra_sects)*SECTOR_SIZE;
	} else
		signed_cluster_no = (s32)cluster_no;

	start = _fatClusterStartSect[drv_no]+_sdInfo[drv_no].spbr.sbpb.sects_per_cluster*(signed_cluster_no-2);
	start += (offset/SECTOR_SIZE);
	
	printf("sect_start = %d,offset = %d\n",start,offset);

	sects_per_block = (1<<(C_SIZE_MULT(drv_no)+2));
	block_start_no = start/sects_per_block;
	
	end = start+(len/SECTOR_SIZE);
	block_end_no = end/sects_per_block;

	bytes_per_block = sects_per_block*SECTOR_SIZE;
	top_size = bytes_per_block-((start-block_start_no*sects_per_block)*SECTOR_SIZE);
	
	printf("sb = %d, eb = %d\n",block_start_no,block_end_no);

	if(block_start_no==block_end_no) {
		ret = card_readBlock(drv_no,block_start_no,bytes_per_block-top_size,buf,len);
	} else {
		bottom_size = (end-block_end_no*sects_per_block)*SECTOR_SIZE;
		if(top_size==bytes_per_block) top_size = 0;

		if(top_size) {
			ret = card_readBlock(drv_no,block_start_no,bytes_per_block-top_size,read_buf,top_size);
			if(ret!=CARDIO_ERROR_READY) return ret;
			
			read_buf += top_size;
			++block_start_no;
		}
		for(i=block_start_no;i<block_end_no;++i) {
			ret = card_readBlock(drv_no,i,0,read_buf,bytes_per_block);
			if(ret!=CARDIO_ERROR_READY) return ret;

			read_buf += bytes_per_block;
		}
		if(bottom_size) {
			ret	= card_readBlock(drv_no,block_end_no,0,read_buf,bottom_size);
			if(ret!=CARDIO_ERROR_READY) return ret;
		}
	}
	return CARDIO_ERROR_READY;
}

boolean card_convertToFATName(const u8* p_name, u8* p_short_name) 
{
	u8 ext[3];
	u8 base[8];
	s32 len;
	s32 extLen = 0;
	s32 baseLen = 0;
	s32 extIndex;
	boolean converted = FALSE;
	boolean ext_converted = FALSE;
	s32 i;

	memset(p_short_name,' ',11);
	p_short_name[11] = '\0';
	len = strlen((char*)p_name);

	/* "." and ".." are special cases */
	if(strcmp((char*)p_name, ".")==0 || strcmp((char*)p_name, "..")==0) {
			memcpy(p_short_name, p_name, len);
			return FALSE;
	}

	/* find extension */
	for(extIndex=len-1;extIndex>=0;--extIndex) {
			if (p_name[extIndex]=='.') 
					break;
	}
	/* convert extension */
	if(extIndex>=0) {
		for(i=extIndex+1; i<len;++i) {
			u8 c;

			c = p_name[i];
			if(c!=' ') {
				ext[extLen] = toupper(c);
				if(++extLen==3) {
					if(p_name[i+1]!=0) ext_converted = TRUE;
					break;
				}
			} else 
				converted = TRUE;
		}
	} else {
		extIndex = len;
	}

	/* convert base name */
	for(i=0;i<extIndex;++i) {
		u8 c;

		c = p_name[i];
		if(c!=' ' && c!='.') {
			base[baseLen] = toupper(c);
			if(++baseLen==8) break;
		} else 
			converted = TRUE;
	}

	if(extIndex>8) converted = TRUE;

	memcpy(p_short_name, base, baseLen);
	if(extLen>0) memcpy(p_short_name + 8, ext, extLen);

	/* if converted to a short name, decorate it */
	if(converted || ext_converted) {
		s32 convertedBaseLen;

		convertedBaseLen = (baseLen>6)?6:baseLen;

		/* find multi-byte character boundary */
		for(i=convertedBaseLen-1;i>=0;i--) {
			if(base[i]<0x80) break;
		}

		if(i>=0) {
			if(!(i&1)) {
				convertedBaseLen--;
				p_short_name[convertedBaseLen+2] = ' ';
			}
		}
    
		p_short_name[convertedBaseLen] = '~';
		p_short_name[convertedBaseLen+1] = '1';

		return TRUE;
	}

	return FALSE;
}

s32 card_getLongName(u32 drv_no, u32 h_parent, u32 short_cluster, u32 offset, u16* p_lname) 
{
	s32 ret;
    u32 i, j, k;
    u32 loop_cnt;
    u32 cluster;
    u32 loffset;
    u32 copy_count;
    u8 file_info[32];
    u32 index = 0;

    cluster = short_cluster;
    loffset = offset;

    loop_cnt = (MAX_FILE_NAME_LEN>>1)/13+1;
    for(i=0;i<loop_cnt;++i) {
        ret = card_findEntryInDirectory(drv_no,FIND_PREVIOUS,h_parent,(cluster<<16)|(loffset&0xffff),file_info,&cluster,&loffset);
        if(ret!= CARDIO_ERROR_READY) {
            if(ret==CARDIO_ERROR_NOTFOUND) {
				/* for the case that file exist in the first valid block */
				if(i == 0) {
					p_lname[0] = 0;
					return CARDIO_ERROR_NOLONGNAME;
                } else
                    break;
            } else 
                return ret;
        }

        if(file_info[11]==0x0f) {
            copy_count = (index+13>(MAX_FILE_NAME_LEN>>1)-2)?(MAX_FILE_NAME_LEN>>1)-2-index:13;
            for(j=0,k=1;j<copy_count;++j,k+=2) {
                if(k==11) k += 3;
                else if(k==26) k += 2;

                p_lname[index++] = ((u16)file_info[k+1]<<8)|file_info[k];
            }

            p_lname[index] = 0;

            if(file_info[0]&0x40) break;
        } else {
            if(i==0) {
                p_lname[0] = 0;
                return CARDIO_ERROR_NOLONGNAME;
            } else 
                return CARDIO_ERROR_INVALIDNAME;
        }
    }

    /* long name must be end with 0x40 flag at the first byte */
    /* if not, it exceeds the MAX_FILE_NAME_LEN     */
    if(!(file_info[0]&0x40))
		return CARDIO_ERROR_FILENAMETOOLONG;

    return CARDIO_ERROR_READY;
}

s32 card_findEntryInDirectory(u32 drv_no,u32 find_mode,F_HANDLE h_parent,u32 var_par,u8* p_info,u32* p_cluster,u32* p_offset) 
{
	s32 ret;
    u32 j;
    u8 short_name[13];
    u8* buf;
	u16 *p_unicode = NULL;
	u16 *p_unicode_read = NULL;
	u32 unicode_namelen = 0;
    u32 offset;
    u32 cluster_size;
    u32 cluster;
    u16 cur_cluster;
    u32 cur_offset;
    u16 end_cluster;
    u16 old_cluster;
    u32 index = 0;
	u32 after_tilde_index = 0;
    u32 max_offset;

	printf("card_findEntryInDirectory()\n");
	
    cluster_size = _sdInfo[drv_no].spbr.sbpb.sects_per_cluster*SECTOR_SIZE;

	printf("cluster_size = %d\n",cluster_size);
	
    /* pre-processing before entering for-loop */
    if(find_mode==FIND_FILE_NAME) {
		p_unicode = (u16*)malloc(MAX_FILE_NAME_LEN+2);
		if(!p_unicode) return CARDIO_ERROR_OUTOFMEMORY;
		
		p_unicode_read = (u16*)malloc(MAX_FILE_NAME_LEN+2);
		if(!p_unicode_read) {
			free(p_unicode);
			return CARDIO_ERROR_OUTOFMEMORY;
		}

		if(card_convertToFATName((const u8*)var_par,short_name)) {
			unicode_namelen = card_convertStrToUni(p_unicode,(u8*)var_par,strlen((char*)var_par));
			card_uniToUpper(p_unicode,p_unicode,unicode_namelen);
			
			for(after_tilde_index=7;after_tilde_index>0;after_tilde_index--) {
				if(short_name[after_tilde_index]=='1' && short_name[after_tilde_index-1]=='~') break;
			}
		} else {
			p_unicode[0] = 0;
			unicode_namelen = 0;
		}
    } else if(find_mode==FIND_FILE_FAT_NAME) {
        memcpy(short_name, (const void*)var_par, 11);
        short_name[11] = '\0';
    }

    /* Use another routine for FIND_PREVIOUS & FIND_NEXT  */
    /* These are not needed to search from the start */
    if((find_mode==FIND_PREVIOUS) || (find_mode==FIND_NEXT)) {
        cur_cluster = (u16)((var_par>>16)&0xffff);
        cur_offset = var_par&0xffff;

        /* check if cur_offset is on the boundary of 32 bytes */
        if(cur_offset&0x1f) return CARDIO_ERROR_INTERNAL;

        if(find_mode==FIND_PREVIOUS) {
            if(cur_offset<32) {
                if(h_parent == ROOT_HANDLE) return CARDIO_ERROR_NOTFOUND;
                else {
                    end_cluster = h_parent&0xffff;
                    while(1) {
                        old_cluster = end_cluster;
                        end_cluster = GET_FAT_TBL(drv_no,end_cluster);

                        if((end_cluster==cur_cluster) || (end_cluster==LAST_CLUSTER) || (end_cluster==UNUSED_CLUSTER))
                                break;
                    }

                    if(end_cluster == cur_cluster) {
                        cur_cluster = old_cluster;
                        cur_offset = cluster_size - 32;
                    }
                    else if(end_cluster == LAST_CLUSTER)
                        return CARDIO_ERROR_NOTFOUND;
                    else if(end_cluster == UNUSED_CLUSTER)
                        return CARDIO_ERROR_INVALIDFAT;
                }
            } else
                cur_offset -= 32;
        } else {   /* FIND_NEXT */
            if(cur_offset>=cluster_size-32) {
                if(h_parent==ROOT_HANDLE) {
                    if(cur_offset >= _fatRootSects[drv_no]*SECTOR_SIZE-32) return CARDIO_ERROR_NOTFOUND;
                    else cur_offset += 32;
                } else {
                    cur_offset = 0;
                    cur_cluster = GET_FAT_TBL(drv_no,cur_cluster);

                    if (cur_cluster == LAST_CLUSTER)
                        return CARDIO_ERROR_NOTFOUND;
                    else if(cur_cluster == UNUSED_CLUSTER)
                        return CARDIO_ERROR_INTERNAL;
                }
            }
            else
                cur_offset += 32;
        }

        *p_cluster = cur_cluster;
        *p_offset = cur_offset;

        ret = card_readCluster(drv_no, cur_cluster, cur_offset, p_info, 32);
        if (ret!=CARDIO_ERROR_READY) return ret;
        
        return CARDIO_ERROR_READY;
    }


    /* find routine except FIND_PREVIOUS & FIND_NEXT */
    buf = card_allocBuffer();
    if(!buf) return CARDIO_ERROR_OUTOFMEMORY;

    cluster = h_parent&0xffff;
    if(h_parent==ROOT_HANDLE) max_offset = _fatRootSects[drv_no]*SECTOR_SIZE;
    else max_offset = cluster_size;
          
	printf("var_par = %s,short_name = %s\n",(u8*)var_par,short_name);
	printf("cluster = %d,max_offset = %d\n",cluster,max_offset);
    while(1) {
        for(offset=0;offset<max_offset;offset+=SECTOR_SIZE) {
			ret = card_readCluster(drv_no, cluster, offset, buf, SECTOR_SIZE);
            if(ret!=CARDIO_ERROR_READY) {
                card_freeBuffer(buf);
                return ret;
            }

            for(j=0;j<SECTOR_SIZE;j+=32) {
                if(find_mode == FIND_FILE_NAME) {
					printf("buf = %s\n",buf+j);
                    if(unicode_namelen) {
                        if((memcmp(short_name,buf+j,after_tilde_index)!=0) 
                           || (memcmp(short_name+8,buf+j+8,3)!= 0))
                                continue;

                        /***
                        * sm_GetLongName() calls sm_FindEntryInDirectory() again. 
                        * But its find_mod is FIND_PREVIOUS, and it is processed before this loop
                        ***/
                        card_getLongName(drv_no, h_parent, cluster, offset+j, p_unicode_read);
                        card_uniToUpper(p_unicode_read, p_unicode_read, unicode_namelen);

                        if(memcmp(p_unicode, p_unicode_read, unicode_namelen<<1)==0) break;
                        else
							continue;
                    } else if(memcmp(short_name,buf+j,11)==0) break;
                } else if(find_mode == FIND_FILE_FAT_NAME) {
                    if(memcmp(short_name,buf+j,11)==0) break;
                } else if (find_mode == FIND_CLUSTER) {
                        if((buf[j]!=0xe5) && ((buf[j+26]|(u16)buf[j+27]<<8)==(u16)var_par) && ((buf[j+11]&0xf)!=0xf)) 
                            break;
                } else if(find_mode == FIND_FILE_INDEX) {
                        /* exclude no file, deleted file, entry for long file name */
                    if((buf[j]!=0) && (buf[j]!=0xe5) && ((buf[j+11]&0xf)!=0xf)) {
                        if(index>=var_par) break;
                        ++index;
                    }
                } else if (find_mode == FIND_SUBDIR_INDEX) {
                    if(buf[j+11]&0x10) {
                        /* exclude no file, deleted file, parent_dir, current_dir */
                        if((buf[j]!=0) && (buf[j]!=0xe5) && (buf[j]!='.') && ((buf[j+11]&0xf)!=0xf)) {
                            if (index >= var_par) break;
                            ++index;
                        }
                    }
                } else if(find_mode==FIND_UNUSED) {
                    if(buf[j]==0) break;
                } else if(find_mode==FIND_DELETED) {
                    if(buf[j] ==0xe5) {
                        u32 count=1;
                        
                        if(var_par>1){
                            for (j+=32;j<SECTOR_SIZE;j+=32) {
                                if(buf[j]==0xe5) count++;
                                else break;

                                if(count>=var_par) break;
                            }
                        }

                        if(count>=var_par) break;
                    }
                } else {
                    card_freeBuffer(buf);
                    return CARDIO_ERROR_INVALIDPARAM;
                }

                if(buf[j]==0) {
                    card_freeBuffer(buf);
                    return CARDIO_ERROR_NOTFOUND;
                }
            }       /* for (j = 0; j < SECTOR_SIZE; j += 32) */

            if(j<SECTOR_SIZE) {
                memcpy(p_info, buf+j,32);
                *p_cluster = cluster;
                *p_offset = offset+j;

                card_freeBuffer(buf);
                return CARDIO_ERROR_READY;
            }
        }       /* for (offset = 0; offset < max_offset; offset += SECTOR_SIZE) */


        if(h_parent==ROOT_HANDLE) break;
        else {
            cluster = GET_FAT_TBL(drv_no,cluster);

            /* this is the case that FAT table is not updated correctly */
            if(cluster==UNUSED_CLUSTER) {
                   card_freeBuffer(buf);
                    return CARDIO_ERROR_INCORRECTFAT;
            }

            if(cluster==LAST_CLUSTER) break;
        }
    }       /* while(1) */

    card_freeBuffer(buf);
    return CARDIO_ERROR_NOTFOUND;
}

s32 card_writeSMInfo(s32 drv_no,sd_info *psm_info)
{
	return -1;
}

s32 card_checkPath(s32 drv_no,const u8 *p_filename,u32 check_mode,F_HANDLE *p_handle)
{
	s32 ret;
	u32 len;
	u32 dummy_cluster,dummy_offset;
	u8 *p_found;
	u8 lname[MAX_FILE_NAME_LEN+1];
	u8 file_info[32];
	F_HANDLE lhandle;
	F_HANDLE lhandle_found;
	F_HANDLE lhandle_old;
	const char *p_str;

	p_str = (char*)p_filename;
	lhandle = lhandle_old = ROOT_HANDLE;

	p_found = strchr(p_str,':');
	if(p_found) {
		p_str = p_found+1;
		printf("p_found = %s, p_str = %s\n",p_found,p_str);
		if(p_str[0]!='\\') return CARDIO_ERROR_INVALIDPARAM;

		++p_str;

		if(p_str[0]==0) {
			if(check_mode==PATH_EXCEPT_LAST) return CARDIO_ERROR_INVALIDPARAM;
			else {
				*p_handle = ROOT_HANDLE;
				return CARDIO_ERROR_READY;
			}
		}

		for(;;) {
			p_found = strchr(p_str,'\\');
			if(!p_found) {
				printf("p_str = %s\n",p_str);
				len = strlen((char*)p_str);
				if(len>MAX_FILE_NAME_LEN) return CARDIO_ERROR_INVALIDPARAM;
				
				if(p_str[0]==0) {
					if(check_mode==PATH_EXCEPT_LAST) *p_handle = lhandle_old;
					else if(check_mode==PATH_FULL) *p_handle = lhandle;
					else return CARDIO_ERROR_INVALIDPARAM;
				} else {
					if(check_mode==PATH_EXCEPT_LAST) *p_handle = lhandle;
					else if(check_mode==PATH_FULL) {
						ret = card_findEntryInDirectory(drv_no,FIND_FILE_NAME,lhandle,(u32)p_str,file_info,&dummy_cluster,&dummy_offset);
						if(ret!=CARDIO_ERROR_READY) return ret;

						lhandle_found = file_info[26]|((u32)file_info[27]<<8);
						*p_handle = lhandle_found;
					} else
						return CARDIO_ERROR_INVALIDPARAM;
				}
				return CARDIO_ERROR_READY;
			}
			
			len = (u32)p_found-(u32)p_str;
			if(len>MAX_FILE_NAME_LEN) return CARDIO_ERROR_INVALIDPARAM;

			memcpy(lname,p_str,len);
			lname[len] = 0;
			printf("lname = %s\n",lname);
			ret = card_findEntryInDirectory(drv_no,FIND_FILE_NAME,lhandle,(u32)lname,file_info,&dummy_cluster,&dummy_offset);
			if(ret!=CARDIO_ERROR_READY) return ret;

			lhandle_found = file_info[26]|((u32)file_info[27]<<8);
			lhandle_old = lhandle;
			lhandle = lhandle_found;
			p_str = p_found+1;
		}
	}
	return CARDIO_ERROR_READY;
}

s32 card_deleteFromOpenedList(s32 drv_no,u32 cluster)
{
    opendfile_list* p_opened;
    u32 i;

    p_opened = _fatOpenedFile[drv_no];
    for(i=0;i<MAX_OPENED_FILE_NUM;++i) {
       if(p_opened==NULL) return CARDIO_ERROR_FILENOTOPENED;
       if(p_opened->cluster==cluster) {
            if (p_opened->prev == NULL) {
                _fatOpenedFile[drv_no] = p_opened->next;
                if(p_opened->next!=NULL) ((opendfile_list*)p_opened->next)->prev = NULL;
            } else {
                ((opendfile_list*)p_opened->prev)->next = p_opened->next;
                if(p_opened->next!=NULL) ((opendfile_list*)p_opened->next)->prev = p_opened->prev;
            }

            if(p_opened->cache.buf!=NULL) free(p_opened->cache.buf);
            free(p_opened);

            break;
        }
        p_opened = p_opened->next;
    }
    if(i>=MAX_OPENED_FILE_NUM) return CARDIO_ERROR_FILENOTOPENED;

    return CARDIO_ERROR_READY;
}

s32 card_getOpenedList(s32 drv_no,u32 cluster,u32 id,opendfile_list **pp_list)
{
	opendfile_list *p_opened;

	p_opened = _fatOpenedFile[drv_no];
	while(p_opened!=NULL) {
		if(p_opened->cluster==cluster && p_opened->id==id) {
			*pp_list = p_opened;
			return CARDIO_ERROR_READY;
		}
		p_opened = (opendfile_list*)p_opened->next;
	}
	return CARDIO_ERROR_FILENOTOPENED;
}

s32 card_addToOpenedFileList(s32 drv_no,opendfile_list* p_list) 
{
    opendfile_list* p_opened;
    u32 i;

    if(_fatOpenedFile[drv_no]==NULL) _fatOpenedFile[drv_no] = p_list;
    else {
        p_opened = _fatOpenedFile[drv_no];
        for(i=0;i<MAX_OPENED_FILE_NUM;++i) {
            if(p_opened->next==NULL) {
                p_opened->next = p_list;

                p_list->prev = p_opened;
                p_list->next = NULL;
                break;
            }

            p_opened = p_opened->next;
        }

        if(i>=MAX_OPENED_FILE_NUM)
                return CARDIO_ERROR_SYSTEMPARAM;
    }

    return CARDIO_ERROR_READY;
}

s32 card_readFromDisk(s32 drv_no,opendfile_list *p_list,void *buf,u32 cnt,u32 *p_cnt)
{
    u32 i;
	s32 ret;
    u32 read_count;
    u32 remaining_count;
    u32 cluster;
    u32 offset;
    u32 cluster_start_count;
    u32 cluster_end_count;
    u32 cluster_size;
    u32 old_cluster;
    u32 copy_count;
    u32 walked_cluster_count;

    read_count = cnt;
    if(p_list->f_ptr+read_count>p_list->size) read_count = p_list->size - p_list->f_ptr;

    *p_cnt = 0;
    if(read_count==0) return CARDIO_ERROR_READY;

    remaining_count = read_count;

    cluster_size = _sdInfo[drv_no].spbr.sbpb.sects_per_cluster*SECTOR_SIZE;
    cluster_start_count = p_list->f_ptr/cluster_size;
    cluster_end_count = (p_list->f_ptr+read_count+cluster_size-1)/cluster_size;
    offset = p_list->f_ptr%cluster_size;

    cluster = p_list->cur_cluster;
    for(i=cluster_start_count;i<cluster_end_count;++i) {
        copy_count = (remaining_count>cluster_size - offset)?(cluster_size-offset):remaining_count;

        ret = card_readCluster(drv_no, cluster, offset, buf, copy_count);
        if(ret!=CARDIO_ERROR_READY) return ret;

        buf = (u8*)buf+copy_count;
        remaining_count -= copy_count;
        offset = 0;

        if(i<cluster_end_count-1) {
            old_cluster = cluster;
            cluster = GET_FAT_TBL(drv_no, old_cluster);
            if(cluster==LAST_CLUSTER) return CARDIO_ERROR_INTERNAL;
        }
    }

    /* update current cluster */
    walked_cluster_count = (p_list->f_ptr%cluster_size+read_count)/cluster_size;
    if(walked_cluster_count>0) {
        while((walked_cluster_count--)>0) {
            p_list->old_cur_cluster = p_list->cur_cluster;
            p_list->cur_cluster = GET_FAT_TBL(drv_no, p_list->cur_cluster);
        }
    }

    p_list->f_ptr += read_count;
    *p_cnt = read_count;
    if(read_count<cnt) return CARDIO_ERROR_EOF;

    return CARDIO_ERROR_READY;
}

void card_prepareFileClose(s32 drv_no, const opendfile_list* p_list) 
{
	s32 ret;
    u32 curCluster;
    u32 lastCluster = 0;
    u32 curSize = 0;
    u32 sectorsPerBlock;
    u32 sectorsPerCluster;
    //const sDEV_INFO* devInfo;
    u32 lsector;
    u32 lblock;

   //ret = smlGetDeviceInfo(drv_no, &devInfo);
   //if(ret!=CARDIO_ERROR_READY) return;

    //sectorsPerBlock = devInfo->SpB;
    sectorsPerCluster = _sdInfo[drv_no].spbr.sbpb.sects_per_cluster;

    curCluster = p_list->cluster;
    while(curCluster!=LAST_CLUSTER) {
        curSize += sectorsPerCluster*SECTOR_SIZE;
        if(curSize>=p_list->size) {
            u32 nextCluster;

            nextCluster = GET_FAT_TBL(drv_no, curCluster);
            if (lastCluster) SET_FAT_TBL(drv_no, curCluster, UNUSED_CLUSTER);
            else {
                SET_FAT_TBL(drv_no, curCluster, LAST_CLUSTER);
                lastCluster = curCluster;
            }
            curCluster = nextCluster;
        }
        else 
			curCluster = GET_FAT_TBL(drv_no, curCluster);
    }

    /* touch the last block */
    lsector = _fatClusterStartSect[drv_no]+lastCluster*sectorsPerCluster+(p_list->size%(sectorsPerCluster*SECTOR_SIZE))/SECTOR_SIZE;
    lblock = lsector / sectorsPerBlock;
   // smlTouchBlock(drv_no, lblock);
}

s32 card_openFile(const char *filename,u32 open_mode,F_HANDLE *p_handle)
{
	s32 ret,drv_no;
	u32 cluster,id;
	u32 dummy_offset;
	F_HANDLE h_dir;
	opendfile_list *p_list;
	u8 file_info[32];
	u8 long_name[MAX_FILE_NAME_LEN+1];
	u8 *p_filename = (u8*)filename;
	
	drv_no = card_getDriveNo(p_filename);
	if(drv_no>=MAX_DRIVE) return CARDIO_ERROR_INVALIDPARAM;

	ret = card_preFAT(drv_no);
	if(ret!=CARDIO_ERROR_READY)
		FAT_RETURN(drv_no,ret);

	ret = card_checkPath(drv_no,p_filename,PATH_EXCEPT_LAST,&h_dir);
	if(ret!=CARDIO_ERROR_READY) 
		FAT_RETURN(drv_no,ret);

	card_extractLastName(p_filename,long_name);
	ret = card_findEntryInDirectory(drv_no,FIND_FILE_NAME,h_dir,(u32)long_name,file_info,&cluster,&dummy_offset);
	if(ret!=CARDIO_ERROR_READY)
		FAT_RETURN(drv_no,ret);
	
	cluster = (u32)file_info[26]|((u32)file_info[27]<<8);
	
	for(id=0;id<MAX_OPENED_FILE_NUM;++id) {
		ret = card_getOpenedList(drv_no,cluster,id,&p_list);
		if(ret==CARDIO_ERROR_FILENOTOPENED) break;
	}
	if(id>=MAX_OPENED_FILE_NUM)
		FAT_RETURN(drv_no,CARDIO_ERROR_FILEOPENED);
	
	p_list = (opendfile_list*)malloc(sizeof(opendfile_list));
	if(p_list==NULL)
		FAT_RETURN(drv_no,CARDIO_ERROR_OUTOFMEMORY);
	
    p_list->prev = NULL;
    p_list->next = NULL;
    p_list->cluster = cluster;
    p_list->h_parent = h_dir;
    p_list->f_ptr = 0;
    p_list->cur_cluster = cluster;
    p_list->old_cur_cluster = 0;
    p_list->size = (u32)file_info[28]|((u32)file_info[29]<<8)|((u32)file_info[30]<<16)|((u32)file_info[31]<<24);
    p_list->mode = open_mode;
    p_list->id = id;

    p_list->cache.f_ptr = 0;
    p_list->cache.cnt = 0;
    p_list->cache.buf = (u8*)malloc(_fatCacheSize[drv_no]);
    if(p_list->cache.buf==NULL) {
        free(p_list);
        FAT_RETURN(drv_no, CARDIO_ERROR_OUTOFMEMORY);
    }

	ret = card_addToOpenedFileList(drv_no,p_list);
	if(ret!=CARDIO_ERROR_READY) {
		free(p_list->cache.buf);
		free(p_list);
		FAT_RETURN(drv_no,ret);
	}

	*p_handle = ((drv_no<<24)&0xff000000)|((p_list->id<<16)&0x00ff0000)|(cluster&0xffff);
	FAT_RETURN(drv_no,CARDIO_ERROR_READY);
}

s32 card_closeFile(F_HANDLE h_file)
{
	s32 ret;
    u32 drv_no;
    u32 cluster;
    opendfile_list* p_list;
    struct tm *time_val;
    u8 file_info[32];
    u32 cluster_no;
    u32 offset;
    u32 id;
	time_t now;

	now = time(NULL);
	time_val = localtime(&now);

    drv_no = (h_file>>24)&0x7f;
    if(drv_no>=MAX_DRIVE)
		return CARDIO_ERROR_INVALIDPARAM;

    cluster = h_file&0xffff;
    id = (h_file>>16)&0xff;

    ret = card_preFAT(drv_no);
    if(ret!=CARDIO_ERROR_READY)
        FAT_RETURN(drv_no, ret);

    ret = card_getOpenedList(drv_no, cluster, id, &p_list);
    if(ret!=CARDIO_ERROR_READY)
        FAT_RETURN(drv_no, ret);
/*
    if(p_list->mode&OPEN_W) {
        if(p_list->cache.cnt!=0)
            sm_WriteCacheToDisk(drv_no, p_list);

        ret = card_findEntryInDirectory(drv_no, FIND_CLUSTER, p_list->h_parent, cluster, file_info, &cluster_no, &offset);
        if(ret!=CARDIO_ERROR_READY) 
            FAT_RETURN(drv_no, ret);

        if(time_val->tm_year > 1980) time_val->tm_year -= 1980;
        else time_val->tm_year = 0;

        // file_info[22-23] <= time
        //      Hour | Min | Sec => 5bit | 6bit | 5bit(half value)
        file_info[22] = ((time_val->tm_sec>>1)&0x1f)|((time_val->tm_min<<5)&0xe0);
        file_info[23] = ((time_val->tm_min>>3)&0x07)|((time_val->tm_hour<<3)&0xf8);

        // file_info[24-25] <= date
        //      Year | Month | Day => 7bit | 4bit | 5bit
        file_info[24] = (time_val->tm_mday&0x1f)|((time_val->tm_mon<<5)&0xe0);
        file_info[25] = ((time_val->tm_mon>>3)&0x01)|((time_val->tm_year<<1)&0xfe);

        // file_info[28-31] <= file size
        file_info[28] = (u8)(p_list->size&0xff);
        file_info[29] = (u8)((p_list->size>>8)&0xff);
        file_info[30] = (u8)((p_list->size>>16)&0xff);
        file_info[31] = (u8)((p_list->size>>24)&0xff);

        ret = smcWriteCluster(drv_no, cluster_no, offset, file_info, 32);
        if (ret!=CARDIO_ERROR_READY) 
            FAT_RETURN(drv_no, ret);

        card_prepareFileClose(drv_no, p_list);

#ifdef FAT_UPDATE_WHEN_FILE_CLOSE
        sm_FATUpdate(drv_no);
#endif
    }
*/
    card_deleteFromOpenedList(drv_no, cluster);

    FAT_RETURN(drv_no, CARDIO_ERROR_READY);
}

s32 card_seekFile(F_HANDLE h_file,u32 seek_mode,s32 offset,s32 *p_oldoffset)
{
	s32 ret;
    u32 drv_no;
    u32 cluster;
    opendfile_list* p_list;
    u32 id;
    u32 cluster_size;

    drv_no = (h_file>>24)&0x7f;
    if(drv_no>=MAX_DRIVE)
		return CARDIO_ERROR_INVALIDPARAM;

    cluster_size = _sdInfo[drv_no].spbr.sbpb.sects_per_cluster*SECTOR_SIZE;

    cluster = h_file&0xffff;
    id = (h_file>>16)&0xff;

    ret = card_preFAT(drv_no);
    if(ret!=CARDIO_ERROR_READY)
        FAT_RETURN(drv_no, ret);

    ret = card_getOpenedList(drv_no,cluster,id,&p_list);
    if(ret!=CARDIO_ERROR_READY)
        FAT_RETURN(drv_no, ret);

    *p_oldoffset = p_list->f_ptr;

    if(seek_mode==FROM_CURRENT) {
        if(offset >= 0) {
            u32 walked_cluster_count;

            /* update current cluster */
            walked_cluster_count = (p_list->f_ptr%cluster_size+offset)/cluster_size;
            if(walked_cluster_count > 0) {
                while ((walked_cluster_count--)>0) {
                    p_list->old_cur_cluster = p_list->cur_cluster;
                    p_list->cur_cluster = GET_FAT_TBL(drv_no,p_list->cur_cluster);
                }
            }

            p_list->f_ptr += offset;
            if(p_list->f_ptr>p_list->size) p_list->f_ptr = p_list->size;
        } else {
            u32 i;

            if(p_list->f_ptr>(u32)(-offset)) p_list->f_ptr += offset;
            else p_list->f_ptr= 0;

            /* update current cluster */
            p_list->cur_cluster = p_list->cluster;
            p_list->old_cur_cluster = 0;
            for(i=cluster_size;i<=p_list->f_ptr;i+=cluster_size) {
                p_list->old_cur_cluster = p_list->cur_cluster;
                p_list->cur_cluster = GET_FAT_TBL(drv_no,p_list->cur_cluster);
            }
        }
    } else if(seek_mode==FROM_BEGIN) {
        u32 i;

        if(offset>=0) {
            p_list->f_ptr = offset;
            if(p_list->f_ptr>p_list->size) p_list->f_ptr = p_list->size;
        } else 
            p_list->f_ptr = 0;

        /* update current cluster */
        p_list->cur_cluster = p_list->cluster;
        p_list->old_cur_cluster = 0;
        for(i=cluster_size;i<=p_list->f_ptr;i+=cluster_size) {
            p_list->old_cur_cluster = p_list->cur_cluster;
            p_list->cur_cluster = GET_FAT_TBL(drv_no,p_list->cur_cluster);
        }
    } else if(seek_mode==FROM_END) {
        u32 i;

        if(offset>=0) p_list->f_ptr = p_list->size;
        else {
            if(p_list->size<(u32)(-offset)) p_list->f_ptr = 0;
            else p_list->f_ptr = p_list->size+offset;
        }

        /* update current cluster */
        p_list->cur_cluster = p_list->cluster;
        p_list->old_cur_cluster = 0;
        for(i=cluster_size;i<=p_list->f_ptr;i+=cluster_size) {
            p_list->old_cur_cluster = p_list->cur_cluster;
            p_list->cur_cluster = GET_FAT_TBL(drv_no,p_list->cur_cluster);
        }
    }
    else
		FAT_RETURN(drv_no,CARDIO_ERROR_INVALIDPARAM);

    FAT_RETURN(drv_no,CARDIO_ERROR_READY);
}

s32 card_readFile(F_HANDLE h_file,void *buf,u32 cnt,u32 *p_cnt)
{
	s32 ret;
    u32 drv_no;
    u32 cluster;
    opendfile_list *p_list;
    u32 head_count, tail_count;
    u32 remaining_count;
    u32 read_result;
    u32 read_sum;
    u32 id;
    u32 offset;
    u32 cache_count;

    drv_no = (h_file>>24)&0x7f;
    if(drv_no>=MAX_DRIVE)
		return CARDIO_ERROR_INVALIDPARAM;

    cluster = h_file&0xffff;
    id = (h_file>>16)&0xff;

    ret = card_preFAT(drv_no);
    if(ret!=CARDIO_ERROR_READY) 
            FAT_RETURN(drv_no, ret);

    ret = card_getOpenedList(drv_no, cluster, id, &p_list);
    if(ret!=CARDIO_ERROR_READY) 
            FAT_RETURN(drv_no, ret);

    *p_cnt = 0;

    /*
     * head_count: read size before the cache area
     * cache_count: read size in the cache area
     * tail_count: read size after the cache area
     */
    head_count = tail_count = 0;
    cache_count = 0;
    remaining_count = cnt;

    if(p_list->cache.cnt) {
        /* p_list->f_ptr is before the cache area */
        if(p_list->f_ptr<p_list->cache.f_ptr) {
            head_count = (p_list->cache.f_ptr - p_list->f_ptr>remaining_count)?remaining_count:(p_list->cache.f_ptr-p_list->f_ptr);
            remaining_count -= head_count;

            if(remaining_count>0) {
                cache_count = (p_list->cache.cnt>remaining_count)?remaining_count:p_list->cache.cnt;
                remaining_count -= cache_count;

                tail_count = remaining_count;
            }
        }
        /* p_list->f_ptr is in the cache area */
        else if(p_list->f_ptr<p_list->cache.f_ptr+p_list->cache.cnt) {
            cache_count = (p_list->cache.cnt-(p_list->f_ptr-p_list->cache.f_ptr)>remaining_count)?remaining_count:(p_list->cache.cnt-(p_list->f_ptr-p_list->cache.f_ptr));
            remaining_count -= cache_count;

            tail_count = remaining_count;
        }
        /* p_list->f_ptr is after the cache area */
        else 
			tail_count = remaining_count;
    }
    else
		head_count = remaining_count;

    read_sum = 0;
    if(head_count) {
        ret = card_readFromDisk(drv_no, p_list, buf, head_count, &read_result);
        if((ret!=CARDIO_ERROR_READY) && (ret!=CARDIO_ERROR_EOF)) 
                FAT_RETURN(drv_no, ret);

        read_sum += read_result;
    }

    if(cache_count) {
        u32 cluster_size;
        u32 walked_cluster_count;

        offset = p_list->f_ptr-p_list->cache.f_ptr;
        memcpy((char*)buf+head_count,p_list->cache.buf+offset,cache_count);

        /* update current cluster */
        cluster_size = _sdInfo[drv_no].spbr.sbpb.sects_per_cluster*SECTOR_SIZE;
        walked_cluster_count = (p_list->f_ptr%cluster_size+cache_count)/cluster_size;
        if(walked_cluster_count>0) {
            while(walked_cluster_count-->0) {
                p_list->old_cur_cluster = p_list->cur_cluster;
                p_list->cur_cluster = GET_FAT_TBL(drv_no, p_list->cur_cluster);
                if(p_list->cur_cluster==LAST_CLUSTER) 
                    FAT_RETURN(drv_no, CARDIO_ERROR_INTERNAL);
            }
        }
        p_list->f_ptr += cache_count;
        read_sum += cache_count;
    }

    if(tail_count) {
        ret = card_readFromDisk(drv_no, p_list, (u8*)buf + head_count + cache_count, tail_count, &read_result);
        if((ret!=CARDIO_ERROR_READY) && (ret!=CARDIO_ERROR_EOF)) 
            FAT_RETURN(drv_no, ret);

        read_sum += read_result;
    }

    *p_cnt = read_sum;

    if(cnt>read_sum) 
        FAT_RETURN(drv_no, CARDIO_ERROR_EOF);

    FAT_RETURN(drv_no, ret);
}





s32 SDCARD_Term(s32 drv_no)
{
	if(drv_no<0 || drv_no>=MAX_DRIVE) return CARDIO_ERROR_FATALERROR;

	printf("SDCARD_Unmount(%d)\n",drv_no);

	card_doUnmount(drv_no);
	return CARDIO_ERROR_READY;
}

void SDCARD_RegisterCallback(u32 drv_no,void (*pFuncIN)(s32),void (*pFuncOUT)(s32))
{
	pfCallbackIN[drv_no] = pFuncIN;
	pfCallbackOUT[drv_no] = pFuncOUT;
}

s32 SDCARD_ReadDir(const char *dirname,u32 entry_start,u32 entry_cnt,dir_entry *dir_buf,u32 *read_cnt)
{
	u32 j,k;
	u32 i,cluster,offset;
	s32 drv_no,ret;
	F_HANDLE h_dir;
	u8 file_info[32];
	u8 *p_dirname = (u8*)dirname;

	drv_no = card_getDriveNo(p_dirname);
	if(drv_no>=MAX_DRIVE) return CARDIO_ERROR_INVALIDPARAM;

	ret = card_preFAT(drv_no);
	if(ret!=CARDIO_ERROR_READY)
		FAT_RETURN(drv_no,ret);

	ret = card_checkPath(drv_no,p_dirname,PATH_FULL,&h_dir);
	if(ret!=CARDIO_ERROR_READY)
		FAT_RETURN(drv_no,ret);

	*read_cnt = 0;
	for(i=0;i<entry_cnt;++i) {
		ret = card_findEntryInDirectory(drv_no,FIND_FILE_INDEX,h_dir,entry_start+i,file_info,&cluster,&offset);
		if(ret!=CARDIO_ERROR_READY) {
			if(ret==CARDIO_ERROR_NOTFOUND) {
				*read_cnt = i;
				FAT_RETURN(drv_no,CARDIO_ERROR_EOF);
			} else
				FAT_RETURN(drv_no,ret);
		}
		
		for(j=0,k=0;j<11;++j) {
			if(file_info[j]!=' ') {
				if(j==8) {
					dir_buf[i].name[k] = '.';
					++k;
				}
				dir_buf[i].name[k] = file_info[j];
				++k;
			}
		}
		dir_buf[i].name[k] = 0;
	}
	
	*read_cnt = entry_cnt;
	FAT_RETURN(drv_no,CARDIO_ERROR_READY);
}