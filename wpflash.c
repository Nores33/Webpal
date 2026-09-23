#include        <stdio.h>
#include        <sys/types.h>
#include        <sys/stat.h>
#include        <fcntl.h>
#include        <termios.h>
#include	<linux/ppdev.h>
#include	<sys/ioctl.h>

int do_write(int, void *, int);
int do_read(int, void *, int);

#define ERROR   (-1)

int     sfd;
int 	nowait = 0;
int     parallel = 0;

#define FLASHSIZE	(8* 1024 * 1024)

char buffer [1024];
char membuf[FLASHSIZE];

struct cmd {
	char type;
	unsigned char length;
	unsigned char addrhi;
	unsigned char addrmed;
	unsigned char addrlow;
};

int addr;

/* Flash Partition definitions */

struct partition {
	char name[32];
	int start;
	int size;
} ;

struct partition part1[] = {
	{ "all"   , 0x000000, 0x100000 },	/* 1M */
	{ "boot"  , 0x000000, 0x006000 },	/* 24K */
	{ "params", 0x006000, 0x002000 },	/* 8K */
	{ "zImage", 0x008000, 0x098000 },	/* 608K */
	{ "root"  , 0x0a0000, 0x060000 },	/* 384K */
	{ "erx"   , 0x020000, 0x0c0000 },	/* erx spot */
	{ ""      , 0x000000, 0x000000 }
};

struct partition part2[] = {
	{ "all"   , 0x000000, 0x200000 },	/* 2M */
	{ "boot"  , 0x000000, 0x008000 },	/* 32K */	
	{ "params", 0x008000, 0x004000 },	/* 16K */
	{ "zImage", 0x00c000, 0x094000 },	/* 592K */	
	{ "root"  , 0x0a0000, 0x160000 },	/* 1408K */
	{ ""      , 0x000000, 0x000000 }
};

int width;
int totalSizeMB;
int size;
int root_start = -1;

struct partition_table {
	int size;
	struct partition *table;
} partition_tables[] = {
	{ 1, part1 },
	{ 2, part2 },
	{ 0 }
};

struct partition *partition_table = part1;
struct partition modified;
struct partition *find_partition(char *);
	

/* This structure defines the sectors
   on the FLASH */

struct sector {
	int start;
	int size;
	int count;
} sectors2258[] = {
	{ 0x000000, 0x004000, 1 },
	{ 0x004000, 0x002000, 2 },
	{ 0x008000, 0x008000, 1 },
	{ 0x010000, 0x010000, 15 },
	{ 0, 0, 0}
};

struct flash_chip {
	int id;
	int size;
	struct sector *sectors;
	char *name;
} flash_chips[] = {
	{0x2258, 512, sectors2258, "29F800B"},
	{0, 0, NULL}
};

struct sector *sector_table = NULL;

int
set_partition_table(int size) 
{
	struct partition_table *p = partition_tables;

	while (p->size) {
		if (p->size == size) {
			partition_table = p->table;
			return (1);
		}
		p++;
	}
	return 0;
}

struct flash_chip * 
find_flash_chip(int id)
{
	struct flash_chip *f = flash_chips;
	
	while (f->size) {
		if (f->id == id) 
			return f;
		f++;
	}
	return NULL;
}

struct partition *
fixup_zImage_partition(struct partition *partition)
{
	struct partition *root;
	int saved_root_start;
	
	/* Look up roots original size and add new space to zImage partition
	   size.  root_start == 0 returns all its space */
	   
	saved_root_start = root_start;
	root_start = -1;
	root = find_partition("root");
	root_start = saved_root_start;
	
	strcpy(modified.name, partition->name);
	modified.start = partition->start;
	if (root_start == 0) 
		modified.size = partition->size + root->size;
	else
		modified.size = partition->size + (root_start - root->start);
	return &modified;
}


struct partition *
fixup_root_partition(struct partition *partition)
{
	struct partition *root;
	
	/* Root start == 0 means it doesn't exist! */
	
	if (root_start == 0)
		return NULL;
		
	strcpy(modified.name, partition->name);
	modified.start = root_start;
	modified.size = root->size - (root_start - root->start);
	return &modified;
}

struct partition *
fixup_partition(struct partition *partition) {

	if (root_start == -1)	/* Nothing to change */
		return partition;
		
	if (strcmp(partition->name, "zImage") == 0)
		return fixup_zImage_partition(partition);
	
	if (strcmp(partition->name, "root") == 0) 
		return fixup_root_partition(partition);

	return partition;
}
		

/* Find a partition name */

struct partition *
find_partition(char *name)
{
	struct partition *partition = partition_table;
	
	while (partition->name[0]) {
		if (strcmp(name, partition->name) == 0)
			return fixup_partition(partition);
		partition++;
	}
	bad_partition();
}

/* Initialize a "cmd" structure */

void
set_cmd(struct cmd *cmd, char type, int addr, int length) {
	cmd->type = type;
	if (length == 256)
		cmd->length = 0;
	else
		cmd->length = length & 0xff;
	cmd->addrhi = (addr >> 16) & 0xff;
	cmd->addrmed = (addr >> 8) & 0xff;
	cmd->addrlow = (addr & 0xff);
	// printf("cmd %02x %02x %02x %02x %02x\n",
	//	cmd->type, cmd->length, cmd->addrhi,
	//	cmd->addrmed, cmd->addrlow);
}

/* Wait for "WebPal Linux Loader" prompt from the Webpal.
   This indicates the WebPal has been booted and is
   waiting for "CONTROL-A" to signal it should jump
   to the programming code */

wait_boot_prompt()
{
	char line[256], *s;

	s = line;
	while (1) {
		do_read(sfd, s, 1);
		if (*s == '\r')
			;
		else if (*s == '\n') {
			*s = 0;
			if (strcmp(line, "Enter CTRL-A to enter programmer.") == 0)
				return 1;
			s = line;
		} else {
			s++;
		}
	}
}

/* Erase a partition */

erase_partition(char *name) {

	struct partition *partition = find_partition(name);
	unsigned int partition_start, partition_end;
	unsigned int sector_start, sector_end;
	int i;
	struct sector *s;
	
	if (partition == NULL) {
		printf("Partition not found\n");
		exit(1);
	}
	
	partition_start = partition->start;
	partition_end = partition->start + partition->size;
	
	/* Loop through the sectors making sure that the paritition
	   is aligned with the sectors.  Not the most efficient way
	   of doing this, but its a simple way.   Since sectors are
	   for 16 flash, we have to modify the sector addresses based
	   on the width of the flash. */
	
	printf("Checking partition and sector alignment");
	fflush(stdout);
	for (s = sector_table; s->size != 0; s++) {
		for (i=0; i < s->count; i++) {
		
			sector_start = (s->start + (s->size * i)) * width / 2;
			sector_end = sector_start + (s->size * width / 2);
				
			if (partition_start >= sector_end)
				continue;
			if (partition_end <= sector_start)
				continue;
			
			if (partition_start > sector_start) {
				printf(
				   "Partition start(%x) > Sector start (%x)\n",
					partition_start, sector_start);
				exit(1);
			}
			if (partition_end < sector_end) {
				printf("Partition end(%x) < Sector end (%x)\n",
					partition_end, sector_end);
				exit(1);
			}
			
			printf(".");
			fflush(stdout);
			
		}
	}
	
	printf("done.\n");
	
	/* Now erase the appropriate sectors on the flash for this partition.
	   Note that although the sector addresses are multiplied by a width
	   factor for the comparison with the partition addresses, the
	   sector addresses are not changed based on the width when issuing
	   the erase sector command. */
	
		
	printf("Erasing %s sectors", partition->name);
	fflush(stdout);
	for (s = sector_table; s->size != 0; s++) {	
		for (i=0; i < s->count; i++) {

			sector_start = (s->start + s->size * i) * width / 2;
			sector_end = sector_start + (s->size * width / 2);
				
			if (partition_start >= sector_end)
				continue;
			if (partition_end <= sector_start)
				continue;	

			erase_sector(s->start + s->size * i);
			printf(".");
			fflush(stdout);
			
		}
	}
	printf("done.\n");
}

/* Program a partition */


program_partition(char *name, char *filename, int hexdump)
{
	struct partition *partition = find_partition(name);
	int addr, maxaddr, maxpgm, addrincr, startaddr;
	int i;

	/* Load data to be programmed in either hex or binary
           format */

	if (hexdump) 
		maxaddr = read_object(filename, membuf, FLASHSIZE);
	else
		maxaddr = read_raw(filename, membuf, FLASHSIZE);

	/* Print statistics and check data fits in partition. */		

	printf("Data size is %d bytes (%d%% of '%s' partition used).\n",  
		maxaddr,
		(maxaddr * 100 + partition->size - 1)/ partition->size,
		partition->name);
	if (maxaddr > partition->size) {
		printf("Data to large for '%s' partition\n", name);
		exit(1);
	}

	/* Erase the partition. */
	   
	erase_partition(name);
	
	/* Calculate write loop variable based on flash width.
	   Address values are in 'flash width' values.
	   Pad maxpgm out to a even number of 'flash width' units. */
	
	startaddr = partition->start / width;
	addrincr = 256 / width;
	maxpgm = (maxaddr + (width - 1)) / width;
			
	for (addr = 0; addr < maxpgm; addr = addr + addrincr) {
	    if (!write_flash(startaddr + addr, &membuf[addr*width], 256)) 
		printf("checksum error %d\n", addr);
	    if ((addr % 4096) == 0) {
		printf("%d\r", addr * width);
		fflush(stdout);
	    }
	}
	printf("%d bytes written.\n", addr * width);
	return 0;
}

/* Read a partition */

read_partition(char *name, char *filename)
{
	struct partition *partition = find_partition(name);
	FILE *file;
	int addr, maxpgm, startaddr, addrincr;

	file = fopen(filename, "w");
	if (file == NULL) {
		fprintf(stderr, "Could not open '%s'\n", filename);
		exit(1);
	}
	
	/* Calculate address values for read loop based on flash width.
	   These addresses are in 'flash width' values.
	   Pad maxpgm out to an even number of 'flash width' units
	   (although the partitions should already be of this size). */	

	addrincr = 256 / width;
	maxpgm = (partition->size + (width - 1))/ width;	
	startaddr = partition->start / width;
	
	printf("Reading partition '%s' (%d bytes) to file '%s'\n",
		partition->name, partition->size, filename);
	for (addr = 0; addr < maxpgm; addr = addr + addrincr) {
	    if (!read_flash(startaddr + addr, membuf, 256)) 
		printf("checksum error\n");
	    fwrite(membuf, 1, 256, file);
	    if ((addr % 4096) == 0) {
		printf("%d\r", addr * width);
		fflush(stdout);
	    }
	}
	printf("%d bytes read.\n", addr * width);
	fclose(file);
	return(0);
}

main(int argc, char **argv) {
        struct termios termios;
        char c;
        int i;
        int cpid;
	int count;
 	int maxcount;
	int version;
	int deviceID1, deviceID2;
	struct flash_chip *f;
	char tempfile[32];
	int fd;
	
	printf("Webpal flash programmer V3.1  1/29/04.\n");

	if (argc < 2)
		usage();
		
	if (strcmp(argv[1], "-p") == 0) {
		argv++;
		argc--;
		parallel = 1;
		nowait = 0;
	} else if (strcmp(argv[1], "-s") == 0) {
		argv++;
		argc--;
		parallel = 0;
		nowait = 0;
	} else if (strcmp(argv[1], "-nowait") == 0) {
		argv++;
		argc--;
		parallel = 0;
		nowait = 2;
	}

	if (parallel) {
		sfd = pp_open();
	} else {
       		if ((sfd = open("/dev/ttyS0", O_RDWR)) == ERROR) {
                	fprintf(stderr, "Can't open terminal\n");
                	exit(1);
        	}
        	termios.c_iflag = IGNBRK | IGNPAR;
        	termios.c_oflag = 0;
        	termios.c_cflag = B115200 | CS8 | CREAD | CLOCAL | CSTOPB;

        	termios.c_lflag = 0;
        	termios.c_cc[VMIN] = 1;
        	termios.c_cc[VTIME] = 0;

        	if (tcsetattr(sfd, TCSANOW, &termios) == ERROR) {
                	fprintf(stderr, "Can't open terminal\n");
                	exit(1);
        	}
	}

	if (!nowait) {
		printf("Reboot Webpal to begin programming...\n");
		if (!parallel)
			wait_boot_prompt();
		do_write(sfd, "\001", 1);
	}

	sleep(1);

	/* Send hello message to programming software
           to see if we can communicate and to get version number */

	printf("Contacting programmer...");
	fflush(stdout);
	
	version = hello();
	printf("detected version %d...done.\n", version);

	/* Send initialize command to programming
           software */

	printf("Initializing flash...");
	fflush(stdout);

	if (!initialize())
		exit(1);
	printf("done\n");
	
	/* Determine device.
	   Use special 'dummy' routine for version 0 programmers */
	
	if (version > 0)	
		device_id(&deviceID1, &deviceID2);
	else
		device_id_v0(&deviceID1, &deviceID2);
		
	/* If both deviceIDs are the same, we have two chips wide, 
	   thus width is 4 bytes.   Otherwise, assume its 2 bytes
	   wide (one chip). For two chip case, send 'double_wide'
	   command to programmer to tell it this */
		
	if (deviceID1 == deviceID2) {
		width = 4;
		double_wide();
	 } else
		width = 2;
		
	/* Now look up the chip based on the deviceID.
	   Save size, sector table, and calculate total size in MB */	
		
	if ((f = find_flash_chip(deviceID2)) == NULL) {
		printf("Chip id unknown %04x\n", deviceID1);
		exit(1);
	}
	
	size = f->size;
	sector_table = f->sectors;
	totalSizeMB = size * width / 1024;
		
	printf("Found %d X %s flash -- %dK X %d bits (total size = %d MByte).\n", 
		width / 2, f->name, size, width * 8, totalSizeMB);
	
	/* Set partition table.
	   Right now, this just uses the total size as key.
	   FIXME: A better key would be the total size and flash chip ID */
	   	
	if (!set_partition_table(totalSizeMB)) {
		printf("No sector table found for %d MByte size", totalSizeMB);
		exit(1);
	}
	
	/* Try and read partition information from params partition on
	   the flash */
	   
	printf("Reading 'params' partition to check partitioning info...\n");
	strcpy(tempfile, "/tmp/wpflashXXXXXX");
	fd = mkstemp(tempfile);
	if (fd != -1) {
		close(fd);  /* So much for race protection with mkstemp :-) */
		read_partition("params", tempfile);
		root_start = parse_flash_params(tempfile);
		// unlink(tempfile);
		switch (root_start) {
		case 0:
			printf("Root partition disabled.\n");
			break;
		case -1:
			printf("No changes from default partition table\n");
			break;
		default:
			printf("Root partition starts at %x.\n", root_start);
			break;
		} 
	} else {
		printf("Could not read partition information from flash\n");
		exit(1);
	}
	
	/* New we are ready to do the operation requested */
	/* Read, write, writehex, or erase */

	if (strcmp(argv[1], "read") == 0) {
	    if (argc != 4)
		usage();
	    return read_partition(argv[2], argv[3]);
	}

	if (strcmp(argv[1], "write") == 0) {
	   if (argc != 4)
		usage();
	    return program_partition(argv[2], argv[3], 0);
	}


	if (strcmp(argv[1], "writehex") == 0) {
	   if (argc != 4)
		usage();
	    return program_partition(argv[2], argv[3], 1);
	}

	if (strcmp(argv[1], "erase") == 0) {
	    if (argc != 3) 
		usage();
	    return erase_partition(argv[2]);
	
	}
	
	/* If we got to here, an invalid operation was specified */

	usage();
}

/* Reads a block of data from the flash.
   addr is in 'flash width' units.
   length is in bytes. */

int
read_flash(int addr, char *buffer, int length)
{
	struct cmd cmd;
	int count;
	int checksum;
	int readl;

	/* Build and send "R" command to programmer.
	   Programmer wants length value in 16 bit quantities so divide by 2 */

	set_cmd(&cmd, 'R', addr, length / 2); 
	do_write(sfd, &cmd, 5);

	/* Read data */

	for (count = 0; count < length + 1; count = count + readl) {
		readl = do_read(sfd, buffer+count, length+1-count);
		if (readl == 0)
			exit;
	}

	/* Check checksum at end of data */

	checksum = 0;
	for (count = 0; count < length; count++) {
		checksum += buffer[count] & 0xff;
		if (checksum >= 256) 
			checksum = (checksum & 0xff) + 1;
	}
	if (checksum != (buffer[length] & 0xff)) {
		printf("checksum error reading data from flash: %d %d\n",
			checksum,  buffer[length] & 0xff);
		return 0;
	} else 
		return 1;

}

/* Write a block of data to the flash.
   addr is is 'flash width' units.
   length is in bytes */

int
write_flash(int addr, char *buffer, int length)
{
	struct cmd cmd;
	int count;
	int checksum;
	int readl;
	char checksumbuf[1];

	/* Build and send "W" command to programmer.
	   Programmer wants length in 16 bit quantities so divide by 2 */

	set_cmd(&cmd, 'W', addr, length / 2);
	do_write(sfd, &cmd, 5);

	/* Send data to be programmed */

	count = do_write(sfd, buffer, length);

	/* Generate and send checksum */

	checksum = 0;
	for (count = 0; count < length; count++) {
		checksum += buffer[count] & 0xff;
		if (checksum >= 256) 
			checksum = (checksum + 1) & 0xff;
	}

	checksumbuf[0] = checksum & 0xff;
	do_write(sfd, checksumbuf, 1);

	/* Read status */

	if (do_read(sfd, checksumbuf, 1) != 1) {
		printf("no resposne to 'write flash' command.\n");
		exit(1);
	}
	if (checksumbuf[0] == 0)
		return 1;
	else {
		printf("write flash @ %08x: command failure: code %x\n",
			addr, checksumbuf[0] & 0xff);
		return 0;
	}

}

/* Send initialize command */

initialize() {
	struct cmd cmd;
	char checksumbuf[1]; 

	set_cmd(&cmd, 'I', 0, 0);
	do_write(sfd, &cmd, 5);

	if (do_read(sfd, checksumbuf, 1) != 1) {
		printf("not response to 'initialize' command\n");
		exit(1);
	}
	if (checksumbuf[0] == 0) {
		return 1;
	} else {
		printf("'initialize' command failure: code %x\n", checksumbuf[0] & 0xff);
		return 0;
	}
}

/* Send device id command.
   Returns the device id of the flash chip(s) in the programmer.
   Not supoorted in Version 0 programmers */

int device_id(int *deviceID1, int *deviceID2) {
	struct cmd cmd;
	unsigned char checksumbuf[5];
	
	set_cmd(&cmd, 'D', 0, 0);
	do_write(sfd, &cmd, 5);
	
	if (do_read(sfd, checksumbuf, 5) != 5) {
		printf("no response to 'device id' command\n");
		exit(1);
	}
	
		
	if (checksumbuf[4] != 0) {
		printf("'device id' command failure: code %d\n",
			checksumbuf[0] & 0xff);
		return 0;
	}
	
	*deviceID1 = checksumbuf[0] *256 + checksumbuf[1];
	*deviceID2 = checksumbuf[2] *256 + checksumbuf[3];	

	return 1;
}

/* Dummy routine to simulate device_id command in programmer that
   do not support it.  Simply returns a fixed device ID matching
   the flash supported by that version of the programmer */

int device_id_v0(int *deviceID1, int *deviceID2)
{
	*deviceID1 = 0xffff;
	*deviceID2 = 0x2258;
	return 1;
}
	

/* Send 32 bit mode command/
   Tells the programmer to program a 32 bit wide flash.
   Note supported on Version 0 programmers. */


double_wide() {
	struct cmd cmd;
	char checksumbuf[1]; 
	
	set_cmd(&cmd, 'T', 0, 0);
	do_write(sfd, &cmd, 5);

	if (do_read(sfd, checksumbuf, 1) != 1) {
		printf("no response to 'double_width' command\n");
		exit(1);
	}
	if (checksumbuf[0] == 0) {
		return 1;
	} else {
		printf("'double width' command failure: code %x\n",
			checksumbuf[0] & 0xff);
		return 0;
	}
}

int parse_flash_params(char *filename)
{
	FILE *fp;
	char line[256];
	
	printf("Parsing partitioning information\n");
	
	fp = fopen(filename, "r");
	if (fp == NULL)
		return -1;
	
	while (fgets(line, 255, fp)) {
		if (strncmp(line, "root_start=", 11) == 0) {
			if (sscanf(line+11, "%x", &root_start) != 1) 
				root_start = -1;
			fclose(fp);
			return root_start;
		}
	}
	fclose(fp);
	return -1;
}

/* Send hello command
   Used to verify communication with programming software.
   Also returned version number of programmer */


int
hello() {
	struct cmd cmd;
	char checksumbuf[1]; 

	set_cmd(&cmd, 'H', 0, 0);
	do_write(sfd, &cmd, 5);

	if (do_read(sfd, checksumbuf, 1) != 1) {
		printf("no response to 'hello' command\n");
		exit(1);
	}
	return (checksumbuf[0]);
}

/* Send erase (whole flash) command */

erase() {
	struct cmd cmd;
	char checksumbuf[1]; 

	set_cmd(&cmd, 'X', 0, 0);
	do_write(sfd, &cmd, 5);

	if (do_read(sfd, checksumbuf, 1) != 1) {
		printf("no response to 'erase' command\n");
		exit(1);
	}
	if (checksumbuf[0] == 0) {
		return 1;
	} else {
		printf("'erase' command failure: code %x\n",
			checksumbuf[0] & 0xff);
		return 0;
	}
}

/* Send erase sector command */

erase_sector(int addr) {
	struct cmd cmd;
	char checksumbuf[1]; 

	set_cmd(&cmd, 'S', addr / 2, 0);
	do_write(sfd, &cmd, 5);

	if (do_read(sfd, checksumbuf, 1) != 1) {
		printf("no resposne to 'erase sector' command\n");
		exit(1);
	}
	if (checksumbuf[0] == 0) {
		return 1;
	} else {
		printf("'erase sector' command failure: code %x\n",
			checksumbuf[0] & 0xff);
		return 0;
	}
}

/* Reads in hex files */

int page = 0;
int maxaddr = 0;

int
hex(char c) {

   if ((c >= '0') && (c <= '9'))
      return c - '0';

   if ((c >= 'a') && (c <= 'f'))
      return c - 'a' + 10;

   if ((c >= 'A') && (c <= 'F'))
      return c - 'A' + 10;

   printf("bad hex digit: '%c'\n", c);
   exit(1);
}

int parse_line(char *buffer, char *mem, int rom_size, int *max_mem) {
   char *c = buffer;
   unsigned int address, word_addr;
   unsigned int length,type;
   unsigned int checksum = 0;
   int count = 0;


   if (*c++ != ':') return -2; 		/* Skip colon */

   length = (hex(*c++) << 4) + hex(*c++);
   checksum = length;

   address = (hex(*c++) << 12)
             + (hex(*c++) << 8)
             + (hex(*c++) << 4)
             + hex(*c++);
   checksum += address & 255;
   checksum += address >> 8;
   if (length == 0) {
      return 0; /* EOF record */
   }

   type = (hex(*c++) << 4) + hex(*c++);
   checksum += type;

   switch (type) {

   case 0:
     while (length--) { 
	 if ((page * 16 + address) >= rom_size) {
		printf("image too large\n");
		exit(1);
	 }  
         mem[page * 16 + address]= (hex(*c++) << 4) + hex(*c++);
         checksum += mem[page * 16 + address] & 0xff;
	 address++;
     }
     if ((page * 16 + address) > *max_mem)
       *max_mem = page * 16 + address;
     break;
   
   case 2:
     if (length != 2)
	return -2;
     page = (hex(*c++) << 12)
             + (hex(*c++) << 8)
             + (hex(*c++) << 4)
             + hex(*c++);
     checksum += page & 0xff;
     checksum += page >> 8;
     break;

   default:
     printf("unknown type %d\n", type);
     return -2;
   
   }
    
   checksum += (hex(*c++) << 4) + hex(*c++);

   if ((checksum & 255) == 0)
	 return count;
   else
	return -1;

}

int
read_object(char *filename, char *memory, int rom_size)
{
   FILE *f;
   char line[80];
   int lines_read = 0;
   int count = 0;
   int bytes ;
   static int max_mem;

   max_mem = 0;

   if(filename)
      f = fopen(filename, "r");
   else
      f = stdin;

   if(filename == NULL) filename = "stdin";
   printf("Reading object file '%s'...",filename);
   fflush(stdout);

   if (f == NULL) {
      printf("failed...cannot open object file\n");
      exit(1);
   }

   while (fgets(line, sizeof(line)-1, f)) {

      lines_read++;
      bytes = parse_line(line, memory,rom_size, &max_mem);
      if (bytes == -1) {
         printf("\nChecksum error in object file on line %d.\n%s\n",
		lines_read, line);
         exit(1);
      }
      if (bytes > 0)
	count += bytes;
      if (bytes == -2)
	printf("\n%s\n",line);

   }

   fclose(f);
   printf("done\n");
   
   return max_mem;  //return highest memory address used
}

/* Reads in binary files */

int
read_raw(char *filename, char *memory, int rom_size)
{
   FILE *f;
   char line[80];
   int lines_read = 0;
   int count = 0;
   int bytes ;
   static int max_mem;

   max_mem = 0;

   if(filename)
      f = fopen(filename, "r");
   else
      f = stdin;

   if(filename == NULL) filename = "stdin";
   printf("Reading raw object file '%s'...",filename);
   fflush(stdout);

   if (f == NULL) {
      printf("failed...cannot open object file\n");
      exit(1);
   }

   while (count = fread(memory, 1, 1024, f)) { 
      memory += count;
      max_mem += count;
    }

   fclose(f);
   printf("done\n");
   
   return max_mem;  //return highest memory address used
}

bad_partition(char *name)
{
	fprintf(stderr, "Unknown partition: %s\n\n", name);
	usage();
}
  
usage() {
	struct partition *partition = partition_table;

	fprintf(stderr, "usage:\n");
	fprintf(stderr, "  wpflash read <partition> <filename>      -- read\n");
	fprintf(stderr, "  wpflash write <partition> <filename>     -- erase/write\n");
	fprintf(stderr, "  wpflash writehex <partition> <filename>  -- erase/write\n");
	fprintf(stderr, "  wpflash erase <partition>                -- erase all flash\n\n");
	fprintf(stderr, "  <partition> is one of:");
	while (partition->name[0]) {
	    fprintf(stderr, " %s", partition->name);
	    partition++;
	}
	fprintf(stderr, "\n");
	
	exit(1);
}

pp_write(int fd, char value)
{
	u_char d = value;
	if (ioctl(fd, PPWDATA, &d) == 1)
		perror("write failed");
}

u_char
pp_read(int fd)
{
	u_char d;
	if (ioctl(fd, PPRSTATUS, &d) == 1)
		perror("read failed");
	return d;
}

int pp_open() 
{
	int i;
	int fd;

	fd = open("/dev/parport0", O_RDWR);
	if (fd < 0) {
		perror("Open parport0");
		exit(1);
	}
	
	i = 0;
	if (ioctl (fd, PPCLAIM, &i) == 1) {
		perror("PPCLAIM failed");
		exit(1);
	}

	pp_write(fd, 0x00);
	return fd;
}

int xfer_byte(int fd, int ivalue)
{
	int ovalue;
	int c;

	/* Input the low nibble */

	while (1) {
		c = pp_read(fd);
		if ((c & 0x80) == 0)
			break;
	}
	c = pp_read(fd);
	/* printf("got first nibble %02x\n", c); */
	ovalue = (c >> 3) & 0x0f;

	/* Output the low nibble */

	pp_write(fd, ivalue & 0xff);
	pp_write(fd, 0x10 | (ivalue & 0xff));

	/* Input the high nibble */

	while (1) {
		c = pp_read(fd);
		if (c & 0x80)
			break;
	}
	c = pp_read(fd);
	/* printf("got second nibble %02x\n", c); */
	ovalue = ovalue | ((c << 1) & 0xf0);

	/* Output the high nibble */
	pp_write(fd, 0x10 | ((ivalue >> 4) & 0x0f));
	pp_write(fd, (ivalue >> 4) & 0x0f);

	/* printf("ivalue %02x ovalue %02x \n", ivalue, ovalue); */

	return ovalue;
}		

int
parallel_write(int fd, char *buffer, int count)
{
	int tempcount = count;
	int thiscount;

	while (count) {
		if (xfer_byte(fd, 0) != 0) {
			printf("Write Sync error\n");
			exit(1);
		}
		if (count < 256)
			thiscount = count;
		else
			thiscount = 255;
		count -= thiscount;
		xfer_byte(fd, thiscount);
		while (thiscount--) {
			xfer_byte(fd, *buffer++);
		}
	}
	return tempcount; 
}

char readbuf[256];
int readcount = 0;
int readoffset = 0;

int 
parallel_read(int fd, char *buffer, int count)
{
	int tempcount = count;

	while (count) {
		if (readcount) {
			*buffer++ = readbuf[readoffset++];
			readcount--;
			count--;
		} else {
			readcount = xfer_byte(fd, 0);
			if (readcount == 0) {
				printf("Read Sync error\n");
				exit(1);
			}
			readoffset = 0;
			while (readoffset != readcount) {
				readbuf[readoffset++] = xfer_byte(fd, 0);
			}
			readoffset = 0;
		}
	}
	return tempcount;
}

int 
do_write(int fd, void *buffer, int count)
{
	if (parallel)
		return parallel_write(fd, buffer, count);
	else
		return write(fd, buffer, count);
}

int 
do_read(int fd, void *buffer, int count) 
{
	if (parallel)
		return parallel_read(fd, buffer, count);
	else
		return read(fd, buffer, count);
}
