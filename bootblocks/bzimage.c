/*
 * Load and execute a bzImage file from the device the 'open_file' and
 * friends use.
 */

#include <stdio.h>
#include "i86_funcs.h"
#include "readfs.h"

int auto_flag = 1;
char * append_line = 0;		/* A preset append line value */

static char * initrd_name = 0;	/* Name of init_ramdisk to load */
static int vga_mode = -1;	/* SVGA_MODE = normal */

static char * read_cmdfile();
static char * input_cmd();

cmd_bzimage(ptr)
char * ptr;
{
   char * image;
   int ch;

   if( (auto_flag=(ptr==0)) ) ptr="";

   while(*ptr == ' ') ptr++;
   image = ptr;
   while(*ptr != ' ' && *ptr) ptr++;
   ch = *ptr;
   *ptr = '\0';
   if( ch ) ptr++;
   while(*ptr == '\n' || *ptr == ' ') ptr++;

   if( *ptr == '\0' ) ptr = 0;
   if( *image )
      bzimage(image, ptr);
   else
      bzimage("vmlinuz", ptr);
}

bzimage(fname, command_line)
char * fname;
char * command_line;
{
   char buffer[1024];
   long len;
   char * ptr;
   int low_sects;
   unsigned int address;

   if( open_file(fname) < 0 )
   {
      if( auto_flag == 0 )
         printf("Cannot find file %s\n", fname);
      return -1;
   }

   printf("Loading %s\n", fname);

   if( read_block(buffer) < 0 || check_magics(buffer) < 0 )
   {
      printf("File %s isn't a bzImage.\n", fname);
      return -1;
   }

   if( boot_mem_top < 0x9500 )
   {
      printf("There must be 640k of boot memory to load Linux\n");
      return -1;
   }

   /* Guestimate how much the uncompressed kernel will use.
    * I expect we could lookup the size in the gzip header but
    * this is probably close enough (3*the size of the bzimage)
    */
   len = file_length() * 3 / 1024;
   if( main_mem_top < len )
   {
      printf("This kernel needs at least %ld.%ldM of main memory\n",
              len/1024, len*10/1024%10);
      return -1;
   }
   if( main_mem_top < 3072 )
      printf("RTFM warning: Linux really needs at least 4MB of memory.\n");

   low_sects = buffer[497] + 1; /* setup sects + boot sector */
   address = 0x900;

   /* load the blocks */
   rewind_file();
   for(len = file_length(); len>0; len-=1024)
   {
      int v;

      printf("%ldk to go \r", len/1024); fflush(stdout);

      v = (bios_khit()&0x7F);
      if( v == 3 || v == 27 )
      {
	 printf("User interrupt!\n");
         bios_getc();
	 return -1;
      }

      if( read_block(buffer) < 0 )
      {
         printf("Error loading %s\n", fname);
	 return -1;
      }

      for(v=0; v<1024; v+=512)
      {
         if( putsect(buffer+v, address) < 0 )
	    return -1;

	 address += 2;

         if( low_sects )
	 {
	    low_sects--;
	    if( low_sects == 0 ) address = 0x1000;
	 }
      }
   }

   /* Yesss, loaded! */
   printf("Loaded, "); fflush(stdout);

   /* Ok, deal with the command line */
   if( build_linuxarg(auto_flag, fname, append_line, command_line) < 0 )
      return -1;

   if( initrd_name )
      if( load_initrd(address) < 0 )
         return -1;

   printf("Starting ...\n");

   if( x86 < 3 )
   {
      printf("RTFM error: Linux-i386 needs a CPU >= 80386\n");
      if( !keep_going() ) return -1;
   }

   if( a20_closed() ) open_a20();
   if( a20_closed() )
   {
      printf("Normal routine for opening A20 Gate failed, Trying PS/2 Bios\n");
      bios_open_a20();
   }
   if( a20_closed() )
   {
      printf("All routines for opening A20 Gate failed, if I can't open it\n");
      printf("then Linux probably can't either!\n");
      if( !keep_going() ) return -1;
   }

   /* Patch setup to deactivate safety switch */
   __set_es(0x9000);
   __poke_es(0x210, 0xFF);

   /* Set SVGA_MODE if not 'normal' */
   if( vga_mode != -1 ) __doke_es(506, vga_mode);

   /* Default boot drive is auto-detected floppy */
   if( __peek_es(508) == 0 ) __poke_es(508, 0x200);

   /* Finally do the deed */
   {
#asm
  ! Kill the floppy motor, needed in case the kernel has no floppy driver.
  mov	dx,#0x3f2
  xor	al, al
  outb

  ! Setup required registers and go ...
  mov	ax,$9000
  mov	bx,$4000-12	! Fix this to use boot_mem_top
  mov	es,ax
  mov	ds,ax
  mov	ss,ax
  mov	sp,bx
  jmpi	0,$9020		! Note SETUPSEG NOT INITSEG
#endasm
   }
}

check_magics(buffer)
char * buffer;
{
   /* Boot sector magic number */
   if( *(unsigned short*)(buffer+510) != 0xAA55 ) return -1;

   /* Setup start */
   if( memcmp(buffer+0x202, "HdrS", 4) != 0 ) return -1;

   /* Setup version */
   if( *(unsigned short*)(buffer+0x206) < 0x200 ) return -1;
   
   /* Check load flags for bzImage */
   if( (buffer[0x211] & 1) == 0 ) return -1;

   /* Code 32 start address */
   if( *(unsigned long*)(buffer+0x214) != 0x100000 ) return -1;

   return 0;
}

putsect(buffer, address)
char * buffer;
unsigned int address;
{
   int rv, tc=3;
retry:
   tc--;
#if 1
   if( x86_emu )
   {
static unsigned int last_address = 0;
      if( address <= last_address )
         printf("Problem %d<=%d\n", address, last_address);
      if( address < 0xA00 )
      {
         int i;
         __set_es(address*16);
	 for(i=0; i<512; i++)
	    __poke_es(i, buffer[i]);
      }
      else
         printf("In EMU can't write to 0x%x\n", address);
      return 0;
   }
#endif
   if( (rv=ext_put(buffer, address, 512)) != 0 )
   {
      switch(rv)
      {
      case 1:
	 printf("Parity error while copying to extended memory\n");
	 break;
      case 2:
	 printf("Interrupt error while copying to extended memory\n");
	 if ( tc>0 ) goto retry;
	 break;
      case 3:
	 printf("BIOS cannot open A20 Gate\n");
	 break;
      case 0x80: case 0x86:
	 printf("BIOS has no extended memory support\n");
	 break;
      default:
	 printf("Error %d while copying to extended memory\n", rv);
	 break;
      }
      if( x86 < 3 )
	 printf("RTFM error: Linux-i386 needs a CPU >= 80386\n");
      else if( x86_emu )
	 printf("RTFM error: Linux-i386 cannot be run in an emulator.\n");
      return -1;
   }
   return 0;
}

static char *
read_cmdfile(iname, extno)
char * iname;
int extno;
{
   char buffer[1024];
   char buf[16];
   long len;
   char * ptr = strchr(iname, '.');

   buf[8] = '\0';
   strncpy(buf, iname, 8);
   switch(extno)
   {
   case 1: strcat(buf, ".cmd"); break;
   case 2: strcat(buf, ".app"); break;
   case 3: strcat(buf, ".dfl"); break;
   default: return 0;
   }

   if( ptr == 0 && open_file(buf) >= 0 )
   {
      /* Ah, a command line ... */
      if( extno == 1 ) printf("Loading %s\n", buf);
      len = file_length();
      if( len > 1023 )
      {
	 printf("Command line file %s truncated to 1023 characters\n", buf);
	 len = 1023;
      }
      if( read_block(buffer) >= 0 )
      {
	 int i;
	 for(i=0; i<len; i++)
	    if( buffer[i] < ' ' ) buffer[i] = ' ';
	 buffer[len] = '\0';

         return strdup(buffer);
      }
   }
   return 0;
}

char *
input_cmd(iname)
char * iname;
{
   char buffer[1024];
   char lbuf[20];
   int cc;

   for(;;)
   {
      printf("%s: ", iname); fflush(stdout);

      cc = read(0, buffer, sizeof(buffer)-1);
      if( cc <= 0 ) return 0;
      buffer[cc] = 0;
      if( cc == 1 && *buffer != '\n' )
      {
	 putchar('\n');
	 cc = (buffer[0] & 0xFF);

         if( cc == 0xAD ) /* ALT-X */
	    return 0;

         sprintf(lbuf, "$%02x", cc);
	 cmd_help(lbuf);
	 continue;
      }
      if( buffer[cc-1] == '\n' ) buffer[cc-1] = '\0';

      break;
   }

   return strdup(buffer);
}

build_linuxarg(auto_flg, image, append, cmd)
int auto_flg;
char *image, *append, *cmd;
{
static char * auto_str  = "auto";
static char * image_str = "BOOT_IMAGE=";
   int len = 0;
   char * ptr, *s, *d;

   char * free_cmd = 0;
   char * free_app = 0;
   char * free_dfl = 0;

   if( append == 0 )
      append = free_app = read_cmdfile(image, 2);

   if( cmd == 0 )
      cmd = free_cmd = read_cmdfile(image, 1);

   if( cmd == 0 )
      free_dfl = read_cmdfile(image, 3);

   close_file();	/* Free up loads a room */

   if( cmd == 0 )
   {
      cmd = free_cmd = input_cmd(image);
      auto_flg = 0;
   }

   if( cmd == 0 )
   {
      printf("Aborted execution\n");
      return -1;
   }
   else if( *cmd == 0 )
      cmd = free_dfl;

   if( auto_flg ) len += strlen(auto_str) + 1;
   if( image ) len += strlen(image_str) + strlen(image) + 1;
   if( append ) len += strlen(append) + 1;
   if( cmd ) len += strlen(cmd) + 1;

   if( len == 0 ) return 0;

   ptr = malloc(len+1);
   if( ptr == 0 )
   {
      printf("Unable to create linux command line\n");
      if( free_cmd ) free(free_cmd);
      if( free_app ) free(free_app);
      if( free_dfl ) free(free_dfl);
      return -1;
   }

   *ptr = 0; ptr[1] = 0;
   if( auto_flg ) { strcat(ptr, " "); strcat(ptr, auto_str); }
   if( image ) { strcat(ptr, " "); strcat(ptr, image_str); strcat(ptr, image); }
   if( append ) { strcat(ptr, " "); strcat(ptr, append); }
   if( cmd ) { strcat(ptr, " "); strcat(ptr, cmd); }

   if( free_cmd ) free(free_cmd);
   if( free_app ) free(free_app);
   if( free_dfl ) free(free_dfl);

   if( (d = malloc(len+1)) == 0 )
   {
      printf("Unable to compact linux command line\n");
      free(ptr);
      return -1;
   }
   *d = '\0';
   for(s=strtok(ptr, " "); s; s=strtok((char*)0, " "))
   {
      if( strncmp(s, "vga=",4) == 0 )
      {
         if( strncmp(s+4, "ask", 3) == 0 )
	    vga_mode = -3;
	 else
	 {
	    s+=4; getnum(&s, &vga_mode);
	 }
      }
      else if( strncmp(s, "ramdisk_file=", 13) == 0 )
      {
	 if( initrd_name ) free(initrd_name);
	 if( s[13] )
            initrd_name = strdup(s+13);
	 else
	    initrd_name = 0;
      }
      else
      {
         strcat(d, " ");
	 strcat(d, s);
      }
   }
   free(ptr); ptr = d;
   len = strlen(ptr);

   if( len > 2048 )
   {
      printf("Linux command line truncated to 2047 characters\n");
      ptr[2048] = 0;
      len = 2048;
   }

   __set_es(0x9000);
   __doke_es(0x0020, 0xA33F);
   __doke_es(0x0022, 0x4000);	

   __movedata(__get_ds(), (unsigned)ptr+1, 0x9000, 0x4000, len);

   free(ptr);

   return 0;
}

keep_going()
{
static int burning = 0;
   char buf[1];
   int cc;

   printf("Keep going ? "); fflush(stdout);

   cc = read(0, buf, 1);
   if( cc == 1 && (*buf == 'Y' || *buf == 'y') )
   {
      printf("Yes, Ok%s\n", burning?"":", burn baby burn!");
      return burning=1;
   }
   printf("No, Ok returning to monitor prompt\n");
   return 0;
}

load_initrd(k_top)
unsigned int k_top;
{
   unsigned int address, rd_start, rd_len;
   long file_len;
   char * fname = initrd_name;

   char buffer[1024];

   /* Top of accessable memory */
   if( main_mem_top >= 15360 ) address = 0xFFFF;
   else                        address = 0x1000 + main_mem_top*4;

   if( *initrd_name == '+' )
   {
      char buf[2];
      fname++;
      close_file();
      printf("Insert root floppy and press return:"); fflush(stdout);
      read(0, buf, 2);
   }

   if( open_file(fname) < 0 )
   {
      printf("Cannot open %s\n", fname);
      return -1;
   }
   file_len = file_length();
   rd_len = (file_len+1023)/1024;

   if( file_len > 15000000L || k_top + rd_len*4 > address )
   {
      printf("Ramdisk file %s is too large to load\n", fname);
      return -1;
   }

   rd_start = address - rd_len*4;
   rd_start &= -16;	/* Page boundry */
   address = rd_start;

   printf("Loading %s at 0x%x00\n", fname, rd_start);

   for( ; rd_len>0 ; rd_len--)
   {
      int v = (bios_khit()&0x7F);
      if( v == 3 || v == 27 )
      {
	 printf("User interrupt!\n");
         bios_getc();
	 return -1;
      }

      printf("%dk to go \r", rd_len); fflush(stdout);
      if( read_block(buffer) < 0 )
      {
         printf("Read error loading ramdisk\n");
	 return -1;
      }
      if( putsect(buffer, address) < 0 ) return -1;
      address+=2;
      if( putsect(buffer+512, address) < 0 ) return -1;
      address+=2;
   }
   printf("Loaded, ");

   /* Tell the kernel where it is */
   {
      long tmp = ((long)rd_start << 8);

      __set_es(0x9000);
      __doke_es(0x218, (unsigned) tmp);
      __doke_es(0x21A, (unsigned)(tmp>>16));

      __doke_es(0x21C, (unsigned) file_len);
      __doke_es(0x21E, (unsigned)(file_len>>16));
   }

   return 0;
}