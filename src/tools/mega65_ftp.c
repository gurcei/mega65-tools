/*

   Upload one or more files to SD card on MEGA65

   Copyright (C) 2018 Paul Gardner-Stephen
   Portions Copyright (C) 2013 Serval Project Inc.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; either version 2
   of the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
   */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/time.h>
#include <time.h>
#include <strings.h>
#include <string.h>
#include <ctype.h>
#include <sys/time.h>
#include <errno.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <stdio.h>
#include <readline/readline.h>
#include <readline/history.h>

#ifdef APPLE
static const int B1000000 = 1000000;
static const int B1500000 = 1500000;
static const int B2000000 = 2000000;
static const int B4000000 = 4000000;
#else
#include <sys/ioctl.h>
#include <linux/serial.h>

#endif
time_t start_time=0;
long long start_usec=0;

int fd=-1;
int cpu_stopped=0;

int open_file_system(void);
int download_slot(int sllot,char *dest_name);
int download_file(char *dest_name,char *local_name,int showClusters);
void show_clustermap(void);
void show_cluster(void);
int show_directory(char *path);
int rename_file(char *name,char *dest_name);
int upload_file(char *name,char *dest_name);
int sdhc_check(void);
int read_sector(const unsigned int sector_number,unsigned char *buffer, int noCacheP);
int load_helper(void);
int stuff_keybuffer(char *s);
int get_pc(void);
unsigned char mega65_peek(unsigned int addr);
int fetch_ram(unsigned long address,unsigned int count,unsigned char *buffer);


// Helper routine for faster sector writing
extern unsigned int helperroutine_len;
extern unsigned char helperroutine[];
int helper_installed=0;
int job_done;
int sectors_written;
int job_status_fresh=0;

int osk_enable=0;

int not_already_loaded=1;

int halt=0;

// 0 = old hard coded monitor, 1= Kenneth's 65C02 based fancy monitor
int new_monitor=0;

int saw_c64_mode=0;
int saw_c65_mode=0;

int first_load=1;
int first_go64=1;

unsigned char viciv_regs[0x100];
int mode_report=0;

int serial_speed=2000000;
char *serial_port="/dev/ttyUSB1";
char *bitstream=NULL;

unsigned char *sd_read_buffer=NULL;
int sd_read_offset=0;

int file_system_found=0;
unsigned int partition_start=0;
unsigned int partition_size=0;
unsigned char sectors_per_cluster=0;
unsigned int sectors_per_fat=0;
unsigned int data_sectors=0;
unsigned int first_cluster=0;
unsigned int fsinfo_sector=0;
unsigned int reserved_sectors=0;
unsigned int fat1_sector=0,fat2_sector=0,first_cluster_sector;

unsigned int syspart_start=0;
unsigned int syspart_size=0;
unsigned int syspart_freeze_area=0;
unsigned int syspart_freeze_program_size=0;
unsigned int syspart_slot_size=0;
unsigned int syspart_slot_count=0;
unsigned int syspart_slotdir_sectors=0;
unsigned int syspart_service_area=0;
unsigned int syspart_service_area_size=0;
unsigned int syspart_service_slot_size=0;
unsigned int syspart_service_slot_count=0;
unsigned int syspart_service_slotdir_sectors=0;

unsigned char mbr[512];
unsigned char fat_mbr[512];
unsigned char syspart_sector0[512];
unsigned char syspart_configsector[512];

int dirent_raw = 0;
int clustermap_start = 0;
int clustermap_count = 0;
int cluster_num = 0;

#ifdef WINDOWS
#include <windows.h>
#undef SLOW_FACTOR
#define SLOW_FACTOR 1
#define SLOW_FACTOR2 1
// #define do_usleep usleep
void do_usleep(__int64 usec) 
{ 
  HANDLE timer; 
  LARGE_INTEGER ft; 

  ft.QuadPart = -(10*usec); // Convert to 100 nanosecond interval, negative value indicates relative time

  timer = CreateWaitableTimer(NULL, TRUE, NULL); 
  SetWaitableTimer(timer, &ft, 0, NULL, NULL, 0); 
  WaitForSingleObject(timer, INFINITE); 
  CloseHandle(timer); 
}

#else
#include <termios.h>
#define do_usleep usleep
#endif

void timestamp_msg(char *msg)
{
  if (!start_time) start_time=time(0);
#ifdef WINDOWS
  fprintf(stderr,"[T+%I64dsec] %s",(long long)time(0)-start_time,msg);
#else
  fprintf(stderr,"[T+%lldsec] %s",(long long)time(0)-start_time,msg);
#endif

  return;
}

#ifdef WINDOWS

void print_error(const char * context)
{
  DWORD error_code = GetLastError();
  char buffer[256];
  DWORD size = FormatMessageA(
      FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_MAX_WIDTH_MASK,
      NULL, error_code, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
      buffer, sizeof(buffer), NULL);
  if (size == 0) { buffer[0] = 0; }
  fprintf(stderr, "%s: %s\n", context, buffer);
}


// Opens the specified serial port, configures its timeouts, and sets its
// baud rate.  Returns a handle on success, or INVALID_HANDLE_VALUE on failure.
HANDLE open_serial_port(const char * device, uint32_t baud_rate)
{
  HANDLE port = CreateFileA(device, GENERIC_READ | GENERIC_WRITE, 0, NULL,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (port == INVALID_HANDLE_VALUE)
  {
    print_error(device);
    return INVALID_HANDLE_VALUE;
  }

  // Flush away any bytes previously read or written.
  BOOL success = FlushFileBuffers(port);
  if (!success)
  {
    print_error("Failed to flush serial port");
    CloseHandle(port);
    return INVALID_HANDLE_VALUE;
  }

  // Configure read and write operations to time out after 1 ms and 100 ms, respectively.
  COMMTIMEOUTS timeouts = { 0 };
  timeouts.ReadIntervalTimeout = 0;
  timeouts.ReadTotalTimeoutConstant = 1;
  timeouts.ReadTotalTimeoutMultiplier = 0;
  timeouts.WriteTotalTimeoutConstant = 100;
  timeouts.WriteTotalTimeoutMultiplier = 0;

  success = SetCommTimeouts(port, &timeouts);
  if (!success)
  {
    print_error("Failed to set serial timeouts");
    CloseHandle(port);
    return INVALID_HANDLE_VALUE;
  }

  DCB state;
  state.DCBlength = sizeof(DCB);
  success = GetCommState(port, &state);
  if (!success)
  {
    print_error("Failed to get serial settings");
    CloseHandle(port);
    return INVALID_HANDLE_VALUE;
  }

  state.fBinary = TRUE;
  state.fDtrControl = DTR_CONTROL_ENABLE;
  state.fDsrSensitivity = FALSE;
  state.fTXContinueOnXoff = FALSE;
  state.fOutX = FALSE;
  state.fInX = FALSE;
  state.fErrorChar = FALSE;
  state.fNull = FALSE;
  state.fRtsControl = RTS_CONTROL_ENABLE;
  state.fAbortOnError = FALSE;
  state.fOutxCtsFlow = FALSE;
  state.fOutxDsrFlow = FALSE;
  state.ByteSize = 8;
  state.StopBits = ONESTOPBIT;
  state.Parity = NOPARITY;

  state.BaudRate = baud_rate;

  success = SetCommState(port, &state);
  if (!success)
  {
    print_error("Failed to set serial settings");
    CloseHandle(port);
    return INVALID_HANDLE_VALUE;
  }

  return port;
}

// Writes bytes to the serial port, returning 0 on success and -1 on failure.
int serialport_write(HANDLE port, uint8_t * buffer, size_t size)
{
  DWORD offset=0;
  DWORD written;
  BOOL success;
  //  printf("Calling WriteFile(%d)\n",size);

  while(offset<size) {  
    success = WriteFile(port, &buffer[offset], size - offset, &written, NULL);
    //  printf("  WriteFile() returned.\n");
    if (!success)
    {
      print_error("Failed to write to port");
      return -1;
    }
    if (written>0) offset+=written;
    if (offset<size) {
      // Assume buffer is full, so wait a little while
      usleep(1000);
    }
  }
  success = FlushFileBuffers(port);
  if (!success) print_error("Failed to flush buffers"); 
  return size;
}

// Reads bytes from the serial port.
// Returns after all the desired bytes have been read, or if there is a
// timeout or other error.
// Returns the number of bytes successfully read into the buffer, or -1 if
// there was an error reading.
SSIZE_T serialport_read(HANDLE port, uint8_t * buffer, size_t size)
{
  DWORD received=0;
  //  printf("Calling ReadFile(%I64d)\n",size);
  BOOL success = ReadFile(port, buffer, size, &received, NULL);
  if (!success)
  {
    print_error("Failed to read from port");
    return -1;
  }
  //  printf("  ReadFile() returned. Received %ld bytes\n",received);
  return received;
}

#else
int serialport_write(int fd, uint8_t * buffer, size_t size)
{
#ifdef __APPLE__
  return write(fd,buffer,size);
#else
  size_t offset=0;
  while(offset<size) {
    int written=write(fd,&buffer[offset],size-offset);
    if (written>0) offset+=written;
    if (offset<size) { usleep(1000);
      //      printf("Wrote %d bytes\n",written);
    }
  }
#endif
}

size_t serialport_read(int fd, uint8_t * buffer, size_t size)
{
  return read(fd,buffer,size);
}

#endif

// From os.c in serval-dna
long long gettime_us()
{
  long long retVal = -1;

  do 
  {
    struct timeval nowtv;

    // If gettimeofday() fails or returns an invalid value, all else is lost!
    if (gettimeofday(&nowtv, NULL) == -1)
    {
      break;
    }

    if (nowtv.tv_sec < 0 || nowtv.tv_usec < 0 || nowtv.tv_usec >= 1000000)
    {
      break;
    }

    retVal = nowtv.tv_sec * 1000000LL + nowtv.tv_usec;
  }
  while (0);

  return retVal;
}

int sd_status_fresh=0;
unsigned char sd_status[16];

int process_char(unsigned char c,int live);


void usage(void)
{
  fprintf(stderr,"MEGA65 cross-development tool for remote access to MEGA65 SD card via serial monitor interface\n");
  fprintf(stderr,"usage: mega65_ftp [-l <serial port>] [-s <230400|2000000|4000000>]  [-b bitstream] [[-c command] ...]\n");
  fprintf(stderr,"  -l - Name of serial port to use, e.g., /dev/ttyUSB1\n");
  fprintf(stderr,"  -s - Speed of serial port in bits per second. This must match what your bitstream uses.\n");
  fprintf(stderr,"       (Older bitstream use 230400, and newer ones 2000000 or 4000000).\n");
  fprintf(stderr,"  -b - Name of bitstream file to load.\n");
  fprintf(stderr,"\n");
  exit(-3);
}

int monitor_sync(void)
{
  /* Synchronise with the monitor interface.
     Send #<token> until we see the token returned to us.
     */

  unsigned char read_buff[8192];

  // Begin by sending a null command and purging input
  char cmd[8192];
  cmd[0]=0x15; // ^U
  cmd[1]='#'; // prevent instruction stepping
  cmd[2]=0x0d; // Carriage return
  do_usleep(20000); // Give plenty of time for things to settle
  slow_write_safe(fd,cmd,3);
  //  printf("Wrote empty command.\n");
  do_usleep(20000); // Give plenty of time for things to settle
  int b=1;
  // Purge input  
  //  printf("Purging input.\n");
  while(b>0) {
    b=serialport_read(fd,read_buff,8192);
    //    if (b>0) dump_bytes(2,"Purged input",read_buff,b);
  }

  for(int tries=0;tries<10;tries++) {
#ifdef WINDOWS
    snprintf(cmd,1024,"#%08x\r",rand());
#else
    snprintf(cmd,1024,"#%08lx\r",random());
#endif
    //    printf("Writing token: '%s'\n",cmd);
    slow_write_safe(fd,cmd,strlen(cmd));

    for(int i=0;i<10;i++) {
      usleep(10000);
      b=serialport_read(fd,read_buff,8192);
      if (b<0) b=0;
      if (b>8191) b=8191;
      read_buff[b]=0;
      //      if (b>0) dump_bytes(2,"Sync input",read_buff,b);

      //      if (b>0) dump_bytes(0,"read_data",read_buff,b);
      if (strstr((char *)read_buff,cmd)) {
        //	printf("Found token. Synchronised with monitor.\n");
        return 0;      
      }
    }
    usleep(10000);
  }
  printf("Failed to synchronise with the monitor.\n");
  return 1;
}

int get_pc(void)
{
  /*
     Get current programme counter value of CPU
     */
  slow_write_safe(fd,"r\r",2);
  do_usleep(50000);
  unsigned char buff[8192];
  int b=serialport_read(fd,buff,8192);
  if (b<0) b=0;
  if (b>8191) b=8191;
  buff[b]=0;
  //  if (b>0) dump_bytes(2,"PC read input",buff,b);
  char *s=strstr((char *)buff,"\n,");
  if (s) return strtoll(&s[6],NULL,16);
  else return -1;
}


int stuff_keybuffer(char *s)
{
  int buffer_addr=0x277;
  int buffer_len_addr=0xc6;

  if (saw_c65_mode) {
    buffer_addr=0x2b0;
    buffer_len_addr=0xd0;
  }

  timestamp_msg("Injecting string into key buffer at ");
  fprintf(stderr,"$%04X : ",buffer_addr);
  for(int i=0;s[i];i++) {
    if (s[i]>=' '&&s[i]<0x7c) fprintf(stderr,"%c",s[i]); else fprintf(stderr,"[$%02x]",s[i]);    
  }
  fprintf(stderr,"\n");

  char cmd[1024];
  snprintf(cmd,1024,"s%x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\rs%x %d\r",
      buffer_addr,s[0],s[1],s[2],s[3],s[4],s[5],s[6],s[7],s[8],s[9],
      buffer_len_addr,(int)strlen(s));
  return slow_write(fd,cmd,strlen(cmd),0);
}


int slow_write(int fd,char *d,int l,int preWait)
{
  // UART is at 2Mbps, but we need to allow enough time for a whole line of
  // writing. 100 chars x 0.5usec = 500usec. So 1ms between chars should be ok.
  //  printf("Writing [%s]\n",d);
  int i;
  usleep(preWait);
  for(i=0;i<l;i++)
  {
    int w=write(fd,&d[i],1);
    while (w<1) {
      usleep(1000);
      w=write(fd,&d[i],1);
    }
    // Only control characters can cause us whole line delays,
    if (d[i]<' ') { usleep(2000); } else usleep(0);
  }
  tcdrain(fd);
  return 0;
}

unsigned char mega65_peek(unsigned int addr)
{
  unsigned char b;
  fetch_ram(addr,1,&b);
  return b;
}

int slow_write_safe(int fd,char *d,int l)
{
  // There is a bug at the time of writing that causes problems
  // with the CPU's correct operation if various monitor commands
  // are run when the CPU is running.
  // Stopping the CPU before and then resuming it after running a
  // command solves the problem.
  // The only problem then is if we have a breakpoint set (like when
  // getting ready to load a program), because we might accidentally
  // resume the CPU when it should be stopping.
  // (We can work around this by using the fact that the new UART
  // monitor tells us when a breakpoint has been reached.
  if (!cpu_stopped)
    slow_write(fd,"t1\r",3,0);
  slow_write(fd,d,l,0);
  if (!cpu_stopped) {
    //    printf("Resuming CPU after writing string\n");
    slow_write(fd,"t0\r",3,0);
  }
  return 0;
}


#define READ_SECTOR_BUFFER_ADDRESS 0xFFD6e00
#define WRITE_SECTOR_BUFFER_ADDRESS 0xFFD6e00

unsigned long long gettime_ms()
{
  struct timeval nowtv;
  // If gettimeofday() fails or returns an invalid value, all else is lost!
  if (gettimeofday(&nowtv, NULL) == -1)
    perror("gettimeofday");
  return nowtv.tv_sec * 1000LL + nowtv.tv_usec / 1000;
}

int stop_cpu(void)
{
  // Stop CPU
  usleep(50000);
  slow_write(fd,"t1\r",3,2500);
  return 0;
}

int restart_hyppo(void)
{
  // Start executing in new hyppo
  if (!halt) {
    usleep(50000);
    slow_write(fd,"g8100\r",6,2500);
    usleep(10000);
    slow_write(fd,"t0\r",3,2500);
  }
  return 0;
}

void print_spaces(FILE *f,int col)
{
  for(int i=0;i<col;i++)
    fprintf(f," ");  
}

int dump_bytes(int col, char *msg,unsigned char *bytes,int length)
{
  print_spaces(stderr,col);
  fprintf(stderr,"%s:\n",msg);
  for(int i=0;i<length;i+=16) {
    print_spaces(stderr,col);
    fprintf(stderr,"%04X: ",i);
    for(int j=0;j<16;j++) if (i+j<length) fprintf(stderr," %02X",bytes[i+j]); else fprintf(stderr,"   ");
    fprintf(stderr," | ");
    for(int j=0;j<16;j++) if (i+j<length) {
      if (bytes[i+j]>=0x20&&bytes[i+j]<0x7f) {
        fprintf(stderr,"%c",bytes[i+j]);
      } else fprintf(stderr,".");
    }
    fprintf(stderr,"\n");
  }

  return 0;
}

int process_line(char *line,int live)
{
  //  printf("[%s]\n",line);
  if (!live) return 0;
  if (strstr(line,"ws h RECA8LHC")) {
    if (!new_monitor) printf("Detected new-style UART monitor.\n");
    new_monitor=1;
  }

  {
    int addr;
    int b[16];
    int gotIt=0;
    unsigned int v[4];
    if (line[0]=='?') fprintf(stderr,"%s\n",line);
    if (sscanf(line,":%x:%08x%08x%08x%08x",
          &addr,&v[0],&v[1],&v[2],&v[3])==5) {
      for(int i=0;i<16;i++) b[i]=(v[i/4]>>( (3-(i&3))*8)) &0xff;
      gotIt=1;
    }
    if (sscanf(line," :%x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
          &addr,
          &b[0],&b[1],&b[2],&b[3],
          &b[4],&b[5],&b[6],&b[7],
          &b[8],&b[9],&b[10],&b[11],
          &b[12],&b[13],&b[14],&b[15])==17) gotIt=1;
    if (gotIt) {
      // printf("Read memory @ $%04x\n",addr);

      if (addr==0xf05) {
        job_done=b[0];
        sectors_written=b[1];
        job_status_fresh=1;
      }
      if (addr==0xffd3680) {
        // SD card status registers
        for(int i=0;i<16;i++) sd_status[i]=b[i];
        // dump_bytes(0,"SDcard status",sd_status,16);
        sd_status_fresh=1;
      }
      else if(addr >= READ_SECTOR_BUFFER_ADDRESS && (addr <= (READ_SECTOR_BUFFER_ADDRESS + 0x200))) {
        // Reading sector card buffer
        int sector_offset=addr-READ_SECTOR_BUFFER_ADDRESS;
        // printf("Read sector buffer 0x%03x - 0x%03x\n",sector_offset,sector_offset+15);
        if (sector_offset<512) {
          if (sd_read_buffer) {
            for(int i=0;i<16;i++) sd_read_buffer[sector_offset+i]=b[i];
          }
          sd_read_offset=sector_offset+16;
        }
      }
    }
  }

  return 0;
}


char line[1024];
int line_len=0;

int process_char(unsigned char c, int live)
{
  //printf("char $%02x\n",c);
  if (c=='\r'||c=='\n') {
    line[line_len]=0;
    if (line_len>0) process_line(line,live);
    line_len=0;
  } else {
    if (line_len<1023) line[line_len++]=c;
  }
  return 0;
}

int process_waiting(int fd)
{
  unsigned char  read_buff[1024];
  int b=read(fd,read_buff,1024);
  while (b>0) {
    int i;
    for(i=0;i<b;i++) {
      process_char(read_buff[i],1);
    }
    b=read(fd,read_buff,1024);    
  }
  return 0;
}

void set_speed(int fd,int serial_speed)
{
  struct termios t;
  if (serial_speed==230400) {
    if (cfsetospeed(&t, B230400)) perror("Failed to set output baud rate");
    if (cfsetispeed(&t, B230400)) perror("Failed to set input baud rate");
  } else if (serial_speed==2000000) {
    if (cfsetospeed(&t, B2000000)) perror("Failed to set output baud rate");
    if (cfsetispeed(&t, B2000000)) perror("Failed to set input baud rate");
  } else if (serial_speed==1000000) {
    if (cfsetospeed(&t, B1000000)) perror("Failed to set output baud rate");
    if (cfsetispeed(&t, B1000000)) perror("Failed to set input baud rate");
  } else if (serial_speed==1500000) {
    if (cfsetospeed(&t, B1500000)) perror("Failed to set output baud rate");
    if (cfsetispeed(&t, B1500000)) perror("Failed to set input baud rate");
  } else {
    if (cfsetospeed(&t, B4000000)) perror("Failed to set output baud rate");
    if (cfsetispeed(&t, B4000000)) perror("Failed to set input baud rate");
  }
  t.c_cflag &= ~PARENB;
  t.c_cflag &= ~CSTOPB;
  t.c_cflag &= ~CSIZE;
  t.c_cflag &= ~CRTSCTS;
  t.c_cflag |= CS8 | CLOCAL;
  t.c_lflag &= ~(ICANON | ISIG | IEXTEN | ECHO | ECHOE);
  t.c_iflag &= ~(BRKINT | ICRNL | IGNBRK | IGNCR | INLCR |
      INPCK | ISTRIP | IXON | IXOFF | IXANY | PARMRK);
  t.c_oflag &= ~OPOST;
  if (tcsetattr(fd, TCSANOW, &t)) perror("Failed to set terminal parameters");

}

int queued_command_count=0;
#define MAX_QUEUED_COMMANDS 64
char *queued_commands[MAX_QUEUED_COMMANDS];

int queue_command(char *c)
{
  if (queued_command_count<MAX_QUEUED_COMMANDS)
    queued_commands[queued_command_count++]=c;
  else {
    fprintf(stderr,"ERROR: Too many commands queued via -c\n");
  }
  return 0;
}

unsigned char show_buf[512];
int show_sector(unsigned int sector_num)
{
  if (read_sector(sector_num,show_buf,0)) {
    printf("ERROR: Could not read sector %d ($%x)\n",sector_num,sector_num);
    return -1;
  }
  dump_bytes(0,"Sector contents",show_buf,512);
  return 0;
}

int execute_command(char *cmd)
{
  unsigned int sector_num;

  if (strlen(cmd)>1000) {
    fprintf(stderr,"ERROR: Command too long\n");
    return -1;
  }
  int slot=0;
  char src[1024];
  char dst[1024];
  if ((!strcmp(cmd,"exit"))||(!strcmp(cmd,"quit"))) {
    printf("Reseting MEGA65 and exiting.\n");
    restart_hyppo();
    exit(0);
  }

  if (sscanf(cmd,"getslot %d %s",&slot,dst)==2) {
    download_slot(slot,dst);
  } else if (sscanf(cmd,"get %s %s",src,dst)==2) {
    download_file(src,dst,0);
  }
  else if (sscanf(cmd,"put %s %s",src,dst)==2) {
    upload_file(src,dst);
  }
  else if (sscanf(cmd,"rename %s %s",src,dst)==2) {
    rename_file(src,dst);
  }
  else if (sscanf(cmd,"sector %d",&sector_num)==1) {
    show_sector(sector_num);
  }
  else if (sscanf(cmd,"sector $%x",&sector_num)==1) {
    show_sector(sector_num);
  } else if (sscanf(cmd,"dirent_raw %d", &dirent_raw) == 1) {
    printf("dirent_raw = %d\n", dirent_raw);
  }
  else if (sscanf(cmd,"dir %s",src)==1) {
    show_directory(src);
  }
  else if (!strcmp(cmd,"dir")) {
    show_directory("/");
  }
  else if (sscanf(cmd,"put %s",src)==1) {
    char *dest=src;
    // Set destination name to last element of source name, if no destination name provided
    for(int i=0;src[i];i++) if (src[i]=='/') dest=&src[i+1];
    upload_file(src,dest);
  }
  else if (sscanf(cmd,"get %s",src)==1) {
    download_file(src,src,0);
  } else if (sscanf(cmd, "clustermap %d %d", &clustermap_start, &clustermap_count)==2) {
    show_clustermap();
  } else if (sscanf(cmd, "clustermap %d", &clustermap_start)==1) {
    clustermap_count = 1;
    show_clustermap();
  } else if (sscanf(cmd,"clusters %s",src)==1) {
    download_file(src,src,1);
  } else if (sscanf(cmd,"cluster %d", &cluster_num)==1) {
    show_cluster();
  } else if (!strcasecmp(cmd,"help")) {
    printf("MEGA65 File Transfer Program Command Reference:\n");
    printf("\n");
    printf("dir [directory] - show contents of current or specified directory.\n");
    printf("put <file> [destination name] - upload file to SD card, and optionally rename it destination file.\n");
    printf("get <file> [destination name] - download file from SD card, and optionally rename it destination file.\n");
    printf("sector <num> - shows a hexdump of the 512-bytes within the specified sector.\n");
    printf("clusters <decimal>|<$hex> - show cluster chain of specified file.\n");
    printf("getslot <slot> <destination name> - download a freeze slot.\n");
    printf("dirent_raw 0|1 - flag to hide/show 32-byte dump of directory entries.\n");
    printf("clustermap <startidx> [<count>] - show cluster-map entries for specified range.\n");
    printf("cluster <num> - dump the entire contents of this cluster.\n");
    printf("exit - leave this programme.\n");
    printf("quit - leave this programme.\n");
  } else {
    fprintf(stderr,"ERROR: Unknown command or invalid syntax. Type help for help.\n");
    return -1;
  }
  return 0;
}

int main(int argc,char **argv)
{
  start_time=time(0);
  start_usec=gettime_us();

  int opt;
  while ((opt = getopt(argc, argv, "b:s:l:c:")) != -1) {
    switch (opt) {
      case 'l': strcpy(serial_port,optarg); break;
      case 's':
                serial_speed=atoi(optarg);
                switch(serial_speed) {
                  case 1000000:
                  case 1500000:
                  case 4000000:
                  case 230400: case 2000000: break;
                  default: usage();
                }
                break;
      case 'b':
                bitstream=strdup(optarg); break;
      case 'c':
                queue_command(optarg); break;
      default: /* '?' */
                usage();
    }
  }  

  if (argc-optind==1) usage();

  errno=0;
  fd=open(serial_port,O_RDWR);
  if (fd==-1) {
    fprintf(stderr,"Could not open serial port '%s'\n",serial_port);
    perror("open");
    exit(-1);
  }
  fcntl(fd,F_SETFL,fcntl(fd, F_GETFL, NULL)|O_NONBLOCK);

#ifndef APPLE

  // And also another way
  struct serial_struct serial;
  ioctl(fd, TIOCGSERIAL, &serial); 
  serial.flags |= ASYNC_LOW_LATENCY;
  ioctl(fd, TIOCSSERIAL, &serial);

  {
    char latency_timer[1024];
    int offset=strlen(serial_port);
    while(offset&&serial_port[offset-1]!='/') offset--;
    snprintf(latency_timer,1024,"/sys/bus/usb-serial/devices/%s/latency_timer",
        &serial_port[offset]);
    int new_latency=999;
    FILE *f=fopen(latency_timer,"r");
    if (f) { fscanf(f,"%d",&new_latency); fclose(f); }
    if (new_latency==1) printf("Successfully set USB serial port to low-latency\n");
    else printf("WARNING: Could not set USB serial port to low latency.  Transfers will be (even) slower.\n");
  }

#endif

  // Load bitstream if file provided
  if (bitstream) {
    char cmd[1024];
    snprintf(cmd,1024,"fpgajtag -a %s",bitstream);
    fprintf(stderr,"%s\n",cmd);
    system(cmd);
    fprintf(stderr,"[T+%lldsec] Bitstream loaded\n",(long long)time(0)-start_time);
  }

  // Set higher speed on serial interface to improve throughput, and make sure
  // we have reset.
  set_speed(fd,2000000);
  //  slow_write(fd,"\r!\r",3,0); usleep(100000);
  slow_write(fd,"\r+9\r",4,5000);
  set_speed(fd,4000000);
  //  slow_write(fd,"\r!\r",3,0); usleep(100000);
  //  set_speed(fd,2000000);  
  //  slow_write(fd,"\r+9\r",4,5000);
  //  set_speed(fd,4000000);

  stop_cpu();

  load_helper();

  sdhc_check();

  if (!file_system_found) open_file_system();

  if (queued_command_count) {
    for(int i=0;i<queued_command_count;i++) execute_command(queued_commands[i]);
    return 0;
  } else {
    char *cmd=NULL;
    using_history();
    while((cmd=readline("MEGA65 SD-card> "))!=NULL) {
      execute_command(cmd);
      add_history(cmd);
      free(cmd);
    }
  }

  return 0;
}

void wait_for_sdready(void)
{
  do {  
    //    long long start=gettime_us();

    // Ask for SD card status
    sd_status[0]=0xff;
    while(sd_status[0]&3) {
      sd_status_fresh=0;
      slow_write(fd,"mffd3680\r",strlen("mffd3680\r"),0);
      while(!sd_status_fresh) process_waiting(fd);
      if (sd_status[0]&3) {
        // Send reset sequence
        printf("SD card not yet ready, so reset it.\n");
        slow_write(fd,"sffd3680 0\rsffd3680 1\r",strlen("sffd3680 0\rsffd3680 1\r"),2000);
        sleep(1);
      }
    }
    //     printf("SD Card looks ready.\n");
    //    printf("wait_for_sdready() took %lld usec\n",gettime_us()-start);
  } while(0);
  return;
}

int wait_for_sdready_passive(void)
{
  int retVal=0;
  do {
    //    long long start=gettime_us();

    int tries=16;
    int sleep_time=1;

    // Ask for SD card status
    sd_status[0]=0xff;
    process_waiting(fd);
    while(sd_status[0]&3) {
      sd_status_fresh=0;
      slow_write(fd,"mffd3680\r",strlen("mffd3680\r"),0);
      while(!sd_status_fresh) process_waiting(fd);
      if ((sd_status[0]&3)==0x03)
      { // printf("SD card error 0x3 - failing\n");
        tries--; if (tries) usleep(sleep_time); else {
          retVal=-1; break; }
        sleep_time*=2;
      }
    }
    // printf("SD Card looks ready.\n");
    //    printf("wait_for_sdready_passive() took %lld usec\n",gettime_us()-start);
  } while(0);
  return retVal;
}

int sdhc=-1;
int onceOnly=1;

int sdhc_check(void)
{
  unsigned char buffer[512];

  sleep(1);
  unsigned char read_buff[8192];
  // flush out any serial data that occurred after the restart.
  serialport_read(fd,read_buff,8192);

  sdhc=-1;

  int r0=read_sector(0,buffer,1);
  int r1=read_sector(1,buffer,1);
  int r200=read_sector(0x200,buffer,1);
  //  printf("%d %d %d\n",r0,r1,r200);
  if (r0||r200) {
    fprintf(stderr,"Could not detect SD/SDHC card\n");
    exit(-3);
  }
  if (r1) sdhc=0; else sdhc=1;
  if (sdhc) fprintf(stderr,"SD card is SDHC\n");
  return sdhc;
}

int fetch_ram(unsigned long address,unsigned int count,unsigned char *buffer)
{
  /* Fetch a block of RAM into the provided buffer.
     This greatly simplifies many tasks.
     */

  unsigned long addr=address;
  unsigned long end_addr;
  char cmd[8192];
  unsigned char read_buff[8192];
  char next_addr_str[8192];
  int ofs=0;

  //  fprintf(stderr,"Fetching $%x bytes @ $%x\n",count,address);

  //  monitor_sync();
  while(addr<(address+count)) {
    if ((address+count-addr)<17) {
      snprintf(cmd,8192,"m%X\r",(unsigned int)addr);
      end_addr=addr+0x10;
    } else {
      snprintf(cmd,8192,"M%X\r",(unsigned int)addr);
      end_addr=addr+0x100;
    }
    //    printf("Sending '%s'\n",cmd);
    slow_write_safe(fd,cmd,strlen(cmd));
    while(addr!=end_addr) {
      snprintf(next_addr_str,8192,"\n:%08X:",(unsigned int)addr);
      int b=serialport_read(fd,&read_buff[ofs],8192-ofs);
      if (b<0) b=0;
      if ((ofs+b)>8191) b=8191-ofs;
      //      if (b) dump_bytes(0,"read data",&read_buff[ofs],b);
      read_buff[ofs+b]=0;
      ofs+=b;
      char *s=strstr((char *)read_buff,next_addr_str);
      if (s&&(strlen(s)>=42)) {
        char b=s[42]; s[42]=0;
        if (0) {
          printf("Found data for $%08x:\n%s\n",
              (unsigned int)addr,
              s);
        } 
        s[42]=b;
        for(int i=0;i<16;i++) {

          // Don't write more bytes than requested
          if ((addr-address+i)>=count) break;

          char hex[3];
          hex[0]=s[1+10+i*2+0];
          hex[1]=s[1+10+i*2+1];
          hex[2]=0;
          buffer[addr-address+i]=strtol(hex,NULL,16);
        }
        addr+=16;

        // Shuffle buffer down
        int s_offset=(long long)s-(long long)read_buff+42;
        bcopy(&read_buff[s_offset],&read_buff[0],8192-(ofs-s_offset));
        ofs-=s_offset;
      }
    }
  }
  if (addr>=(address+count)) {
    //    fprintf(stderr,"Memory read complete at $%lx\n",addr);
    return 0;
  }
  else {
    fprintf(stderr,"ERROR: Could not read requested memory region.\n");
    exit(-1);
    return 1;
  }
}

unsigned char ram_cache[512*1024+255];
unsigned char ram_cache_valids[512*1024+255];
int ram_cache_initialised=0;

int fetch_ram_invalidate(void)
{
  ram_cache_initialised=0;
  return 0;
}

int fetch_ram_cacheable(unsigned long address,unsigned int count,unsigned char *buffer)
{
  if (!ram_cache_initialised) {
    ram_cache_initialised=1;
    bzero(ram_cache_valids,512*1024);
    bzero(ram_cache,512*1024);
  }
  if ((address+count)>=(512*1024)) {
    return fetch_ram(address,count,buffer);
  }

  // See if we need to fetch it fresh
  for(int i=0;i<count;i++) {
    if (!ram_cache_valids[address+i]) {
      // Cache not valid here -- so read some data
      printf("."); fflush(stdout);
      //      printf("Fetching $%08x for cache.\n",address);
      fetch_ram(address,256,&ram_cache[address]);
      for(int j=0;j<256;j++) ram_cache_valids[address+j]=1;

      bcopy(&ram_cache[address],buffer,count);
      return 0;
    }
  }

  // It's valid in the cache
  bcopy(&ram_cache[address],buffer,count);
  return 0;

}


int detect_mode(void)
{
  uint8_t read_buff[8192];
  /*
     Set saw_c64_mode or saw_c65_mode according to what we can discover. 
     We can look at the C64/C65 charset bit in $D030 for a good clue.
     But we also really want to know that the CPU is in the keyboard 
     input loop for either of the modes, if possible. OpenROMs being
     under development makes this tricky.
     */
  saw_c65_mode=0;
  saw_c64_mode=0;

  // flush out any serial data that occurred after the restart.
  serialport_read(fd,read_buff,8192);

  unsigned char mem_buff[8192];
  fetch_ram(0xffd3030,1,mem_buff);
  while(mem_buff[0]&0x01) {
    fprintf(stderr,"Waiting for MEGA65 KERNAL/OS to settle...\n");
    do_usleep(200000);
    fetch_ram(0xffd3030,1,mem_buff);    
  }

  // Wait for HYPPO to exit
  int d054=mega65_peek(0xffd3054);
  while(d054&7) {
    do_usleep(50000);
    d054=mega65_peek(0xffd3054);
  }


  //  printf("$D030 = $%02X\n",mem_buff[0]);
  if (mem_buff[0]==0x64) {
    // Probably C65 mode
    int in_range=0;
    // Allow more tries to allow more time for ROM checksum to finish
    // or boot attempt from floppy to finish
    for (int i=0;i<10;i++) {
      int pc=get_pc();
      if (pc>=0xe1a0&&pc<=0xe1b4) in_range++; else {
        // C65 ROM does checksum, so wait a while if it is in that range
        if (pc>=0xb000&&pc<0xc000) sleep(1);
        // Or booting from internal drive is also slow
        if (pc>=0x9c00&&pc<0x9d00) sleep(1);
        // Or something else it does while booting
        if (pc>=0xfeb0&&pc<0xfed0) sleep(1);
        else {
          //	  fprintf(stderr,"Odd PC=$%04x\n",pc);
          do_usleep(100000);
        }
      }
    }
    if (in_range>3) {
      // We are in C65 BASIC main loop, so assume it is C65 mode
      saw_c65_mode=1;
      timestamp_msg("");
      fprintf(stderr,"CPU in C65 BASIC 10 main loop.\n");
      return 0;
    }
  } else if (mem_buff[0]==0x00) {
    // Probably C64 mode
    int in_range=0;
    for (int i=0;i<5;i++) {
      int pc=get_pc();
      // XXX Might not work with OpenROMs?
      if (pc>=0xe5cd&&pc<=0xe5d5) in_range++;
      else {
        //	printf("Odd PC=$%04x\n",pc);
        usleep(100000);
      }
    }
    if (in_range>3) {
      // We are in C64 BASIC main loop, so assume it is C65 mode
      saw_c64_mode=1;
      timestamp_msg("");
      fprintf(stderr,"CPU in C64 BASIC 2 main loop.\n");
      return 0;
    }
  }
  printf("Could not determine C64/C65/MEGA65 mode.\n");
  return 1;
}


int load_helper(void)
{
  int retVal=0;
  do {
    if (!helper_installed) {      
      // Install helper routine
      char cmd[1024];

      detect_mode();

      if ((!saw_c64_mode)) {
        printf("Trying to switch to C64 mode...\n");
        monitor_sync();
        stuff_keybuffer("GO64\rY\r");    
        saw_c65_mode=0;
        do_usleep(100000);
        detect_mode();
        while (!saw_c64_mode) {
          fprintf(stderr,"WARNING: Failed to switch to C64 mode.\n");
          monitor_sync();
          stuff_keybuffer("GO64\rY\r");    
          do_usleep(100000);
          detect_mode();
        }
      }

      snprintf(cmd,1024,"t1\r");
      slow_write(fd,cmd,strlen(cmd),500);

      snprintf(cmd,1024,"l0801 %x\r",0x801+helperroutine_len-2);
      slow_write(fd,cmd,strlen(cmd),500);
      usleep(10000); // give uart monitor time to get ready for the data
      process_waiting(fd);
      int offset=2;
      while(offset<helperroutine_len) {
        int written=write(fd,&helperroutine[offset],helperroutine_len-offset);
        if (written!=helperroutine_len) {
          usleep(10000);
        }
        offset+=written;
      }
      slow_write(fd,"\r",1,500);
      process_waiting(fd);

      // Launch helper programme
      sleep(1);
      snprintf(cmd,1024,"g080d\r");
      slow_write(fd,cmd,strlen(cmd),500);
      snprintf(cmd,1024,"t0\r");
      slow_write(fd,cmd,strlen(cmd),500);

      helper_installed=1;
      printf("\nNOTE: Fast SD card access routine installed.\n");

    }
  } while(0);
  return retVal;
}

int data_byte_count=0;
uint8_t queue_jobs=0;
uint16_t queue_addr=0xc001;
uint8_t queue_read_data[1024*1024];
uint32_t queue_read_len=0;

uint8_t queue_cmds[0x0fff];

uint8_t q_rle_count=0,q_raw_count=0;

void queue_data_decode(uint8_t v)
{
  if (0)  fprintf(stderr,"Decoding $%02x, rle_count=%d, raw_count=%d, data_byte_count=$%04x\n",
      v,q_rle_count,q_raw_count,data_byte_count);
  if (q_rle_count) {
    //    fprintf(stderr,"$%02x x byte $%02x\n",q_rle_count,v);
    data_byte_count-=q_rle_count;
    for(int i=0;i<q_rle_count;i++) {
      if (queue_read_len<1024*1024) queue_read_data[queue_read_len++]=v;
    }
    q_rle_count=0;
  } else if (q_raw_count) {
    //    fprintf(stderr,"Raw byte $%02x\n",v);
    if (queue_read_len<1024*1024) queue_read_data[queue_read_len++]=v;
    if (data_byte_count) data_byte_count--;
    q_raw_count--;
  } else {
    //    fprintf(stderr,"Data code $%02x\n",v);
    if (v&0x80) {
      q_rle_count=v&0x7f;      
      //      fprintf(stderr,"RLE of $%02x bytes\n",q_rle_count);
    } else {
      q_raw_count=v&0x7f;
      //      fprintf(stderr,"$%02x raw bytes\n",q_raw_count);

    }
  }
}


void queue_add_job(uint8_t *j,int len)
{
  char cmd[1024];
  int b;
  uint8_t read_buff[8192];

  b=1;
  while(b>0) {
    b=serialport_read(fd,read_buff,8192);
    //    if (b>0) dump_bytes(2,"Purged input",read_buff,b);
  }

  bcopy(j,&queue_cmds[queue_addr-0xc001],len);
  queue_jobs++;
  queue_addr+=len;
  //  printf("remote job queued.\n");

  b=1;
  while(b>0) {
    b=serialport_read(fd,read_buff,8192);
    //    if (b>0) dump_bytes(2,"Purged input",read_buff,b);
  }


}

void job_process_results(void)
{
  long long now =gettime_us();
  queue_read_len=0;
  uint8_t buff[8192];

  uint8_t recent[32];

  data_byte_count=0;

  int debug_rx=0;

  while (1) {
    int b=read(fd,buff,8192);
    if (b<1) usleep(0);
    if (b>0) if (debug_rx) dump_bytes(0,"jobresponse",buff,b);
    for(int i=0;i<b;i++) {
      // Keep rolling window of most recent chars for interpretting job
      // results
      if (data_byte_count)
      {
        queue_data_decode(buff[i]);
      } else {
        bcopy(&recent[1],&recent[0],30);
        recent[30]=buff[i];
        recent[31]=0;
        if (!strncmp(&recent[30-10],"FTBATCHDONE",11)) {
          long long endtime =gettime_us();
          if (debug_rx) printf("%lld: Saw end of batch job after %lld usec\n",endtime-start_usec,endtime-now);
          //	  dump_bytes(0,"read data",queue_read_data,queue_read_len);
          return;
        }
        if (!strncmp(recent,"FTJOBDONE:",10)) {
          int jn=atoi((char *)&recent[10]);	  
          if (debug_rx) printf("Saw job #%d completion.\n",jn);	  
        }
        int j_addr,n;
        uint32_t transfer_size;
        int fn=sscanf(recent,"FTJOBDATA:%x:%x:%n",&j_addr,&transfer_size,&n);
        if (fn==2) {
          if (debug_rx)
            printf("Spotted job data: Reading $%x bytes of data, offset %d,"
                " %02x %02x\n",transfer_size,n,
                recent[n],recent[n+1]
                );
          q_rle_count=0; q_raw_count=0;
          data_byte_count=transfer_size;
          // Don't forget to process the bytes we have already injested
          for(int k=n;k<=30;k++) {
            if (data_byte_count) {
              queue_data_decode(recent[k]);
            }
          }
        }
      }
    }
  }
}

void queue_execute(void)
{
  char cmd[1024];

  long long start = gettime_us();

  // Push queued jobs in on go
  sprintf(cmd,"l%x %x\r",0xc001,queue_addr);
  slow_write(fd,cmd,strlen(cmd),0);
  // give serial uart time to get ready
  // (and make sure we end up in a different USB packet to the command)
  usleep(1000); 
  serialport_write(fd,queue_cmds,queue_addr-0xc001);
  usleep(3*(queue_addr-0xc001));

  sprintf(cmd,"sc000 %x\r",queue_jobs);
  slow_write(fd,cmd,strlen(cmd),0);
  long long end = gettime_us();
  //  printf("%lld Executing queued jobs (took %lld us to dispatch)\n",end-start_usec,end-start);

  job_process_results();
  queue_addr=0xc001;
  queue_jobs=0;
}

uint32_t write_buffer_offset=0;
uint8_t write_data_buffer[65536];
uint32_t write_sector_numbers[65536/512];
uint8_t write_sector_count=0;

void queue_physical_write_sector(uint32_t sector_number,uint32_t mega65_address)
{
  uint8_t job[9];
  job[0]=0x02;
  job[5]=sector_number>>0;
  job[6]=sector_number>>8;
  job[7]=sector_number>>16;
  job[8]=sector_number>>24;
  job[1]=mega65_address>>0;
  job[2]=mega65_address>>8;
  job[3]=mega65_address>>16;
  job[4]=mega65_address>>24;
  queue_add_job(job,9);
}


int execute_write_queue(void)
{
  int retVal=0;
  do {
    char cmd[1024];
    printf("Executing write queue with %d sectors in the queue (write_buffer_offset=$%08x)\n",
        write_sector_count,write_buffer_offset);
    snprintf(cmd,1024,"l%x %x\r",0x50000,(0x50000+write_buffer_offset)&0xffff);
    //    printf("CMD: '%s'\n",cmd);
    slow_write(fd,cmd,strlen(cmd),1000);
    usleep(5000);
    int offset=0;
    while (offset<write_buffer_offset)
    {
      int written=write(fd,&write_data_buffer[offset],write_buffer_offset-offset);
      if (written>0) offset+=written;
      else usleep(0);
    }
    usleep(3*write_buffer_offset);

    // XXX - Sort sector number order and merge consecutive writes into
    // multi-sector writes would be a good idea here.
    for(int i=0;i<write_sector_count;i++)
    {
      queue_physical_write_sector(write_sector_numbers[i],0x50000+(i<<9));
    }
    queue_execute();

    // Reset write queue
    write_buffer_offset=0;
    write_sector_count=0;
  } while (0);
  return retVal;
}

void queue_write_sector(uint32_t sector_number, uint8_t *buffer)
{
  // Merge writes to same sector
  for (int i=0;i<write_sector_count;i++) {
    if (sector_number==write_sector_numbers[i]) {
      printf("Updating sector $%08x while in the write queue.\n",sector_number);
      bcopy(buffer,&write_data_buffer[i<<9],512);
      return;
    }
  }


  // Purge pending jobs when they are banked up
  // (only 32KB at a time, as the l command for fast pushing data
  // can't do 64KB
  if (write_buffer_offset>=32768) execute_write_queue();

  bcopy(buffer,&write_data_buffer[write_buffer_offset],512);
  write_buffer_offset+=512;
  write_sector_numbers[write_sector_count]=sector_number;
  write_sector_count++;

}  

void queue_read_sector(uint32_t sector_number,uint32_t mega65_address)
{
  uint8_t job[9];
  job[0]=0x01;
  job[5]=sector_number>>0;
  job[6]=sector_number>>8;
  job[7]=sector_number>>16;
  job[8]=sector_number>>24;
  job[1]=mega65_address>>0;
  job[2]=mega65_address>>8;
  job[3]=mega65_address>>16;
  job[4]=mega65_address>>24;
  queue_add_job(job,9);
}

void queue_read_mem(uint32_t mega65_address,uint32_t len)
{
  uint8_t job[9];
  job[0]=0x11;
  job[1]=mega65_address>>0;
  job[2]=mega65_address>>8;
  job[3]=mega65_address>>16;
  job[4]=mega65_address>>24;
  job[5]=len>>0;
  job[6]=len>>8;
  job[7]=len>>16;
  job[8]=len>>24;
  queue_add_job(job,9);
}





#define SECTOR_CACHE_SIZE 4096
int sector_cache_count=0;
unsigned char sector_cache[SECTOR_CACHE_SIZE][512];
unsigned int sector_cache_sectors[SECTOR_CACHE_SIZE];

// XXX - DO NOT USE A BUFFER THAT IS ON THE STACK OR BAD BAD THINGS WILL HAPPEN
int read_sector(const unsigned int sector_number,unsigned char *buffer,int noCacheP)
{
  int retVal=0;
  do {

    int cachedRead=0;

    if (!noCacheP) {
      for(int i=0;i<sector_cache_count;i++) {
        if (sector_cache_sectors[i]==sector_number) {
          bcopy(sector_cache[i],buffer,512);
          retVal=0; cachedRead=1; break;
        }
      }
    }

    if (cachedRead) break;


    // Do read using new remote job queue mechanism that is hopefully
    // lower latency than the old way
    // Request multiple sectors at once to make it more efficient
    int batch_read_size=16;

    for(int n=0;n<batch_read_size;n++)
      queue_read_sector(sector_number+n,0x40000+(n<<9));
    queue_read_mem(0x40000,512*batch_read_size);
    queue_execute();

    for(int n=0;n<batch_read_size;n++) {
      bcopy(&queue_read_data[n<<9],buffer,512);
      //      printf("Sector $%08x:\n",sector_number+n);
      //      dump_bytes(3,"read sector",buffer,512);

      // Store in cache / update cache
      int i;
      for(i=0;i<sector_cache_count;i++) 
        if (sector_cache_sectors[i]==sector_number+n) break;
      if (i<SECTOR_CACHE_SIZE) {
        bcopy(buffer,sector_cache[i],512);
        sector_cache_sectors[i]=sector_number+n;
      }
      if (sector_cache_count<(i+1)) sector_cache_count=i+1;
    }

    // Make sure to return the actual sector that was asked for
    bcopy(&queue_read_data[0],buffer,512);

  } while(0);
  if (retVal) printf("FAIL reading sector %d\n",sector_number);
  return retVal;

}

unsigned char verify[512];

int write_sector(const unsigned int sector_number,unsigned char *buffer)
{
  int retVal=0;
  do {
    // With new method, we write the data, then schedule the write to happen with a job
    char cmd[1024];
    // Clear pending input first
    int b=1;
    while(b>0){
      b=serialport_read(fd,cmd,1024);
      //      if (b) dump_bytes(3,"write_sector() flush data",cmd,b);
    }

    queue_write_sector(sector_number,buffer);

    // Store in cache / update cache
    int i;
    for(i=0;i<sector_cache_count;i++) 
      if (sector_cache_sectors[i]==sector_number) break;
    if (i<SECTOR_CACHE_SIZE) {
      bcopy(buffer,sector_cache[i],512);
      sector_cache_sectors[i]=sector_number;
    }
    if (sector_cache_count<(i+1)) sector_cache_count=i+1;


  } while(0);
  if (retVal) printf("FAIL writing sector %d\n",sector_number);
  return retVal;

}

int open_file_system(void)
{
  int retVal=0;
  do {
    if (read_sector(0,mbr,0)) {
      fprintf(stderr,"ERROR: Could not read MBR\n");
      retVal=-1;
      break;
    }

    for(int i=0;i<4;i++) {
      unsigned char *part_ent = &mbr[0x1be + (i*0x10)];
      // dump_bytes(0,"partent",part_ent,16);
      if (part_ent[4]==0x0c||part_ent[4]==0x0b) {
        partition_start=part_ent[8]+(part_ent[9]<<8)+(part_ent[10]<<16)+(part_ent[11]<<24);
        partition_size=part_ent[12]+(part_ent[13]<<8)+(part_ent[14]<<16)+(part_ent[15]<<24);
        printf("Found FAT32 partition in partition slot %d : start sector=$%x, size=%d MB\n",
            i,partition_start,partition_size/2048);
      }
      if (part_ent[4]==0x41) {
        syspart_start=part_ent[8]+(part_ent[9]<<8)+(part_ent[10]<<16)+(part_ent[11]<<24);
        syspart_size=part_ent[12]+(part_ent[13]<<8)+(part_ent[14]<<16)+(part_ent[15]<<24);
        printf("Found MEGA65 system partition in partition slot %d : start sector=$%x, size=%d MB\n",
            i,syspart_start,syspart_size/2048);	
      }
    }

    if (syspart_start) {
      // Ok, so we know where the partition starts, so now find the FATs
      if (read_sector(syspart_start,syspart_sector0,0)) {
        printf("ERROR: Could not read system partition sector 0\n");
        retVal=-1;
        break;
      }
      if (strncmp("MEGA65SYS00",(char *)&syspart_sector0[0],10)) {
        printf("ERROR: MEGA65 System Partition is missing MEGA65SYS00 marker.\n");
        dump_bytes(0,"SYSPART Sector 0",syspart_sector0,512);
        retVal=-1;
        break;
      }
      syspart_freeze_area=syspart_sector0[0x10]+(syspart_sector0[0x11]<<8)+(syspart_sector0[0x12]<<16)+(syspart_sector0[0x13]<<24);
      syspart_freeze_program_size=syspart_sector0[0x14]+(syspart_sector0[0x15]<<8)+(syspart_sector0[0x16]<<16)+(syspart_sector0[0x17]<<24);
      syspart_slot_size=syspart_sector0[0x18]+(syspart_sector0[0x19]<<8)+(syspart_sector0[0x1a]<<16)+(syspart_sector0[0x1b]<<24);
      syspart_slot_count=syspart_sector0[0x1c]+(syspart_sector0[0x1d]<<8);
      syspart_slotdir_sectors=syspart_sector0[0x1e]+(syspart_sector0[0x1f]<<8);
      syspart_service_area=syspart_sector0[0x20]+(syspart_sector0[0x21]<<8)+(syspart_sector0[0x22]<<16)+(syspart_sector0[0x23]<<24);
      syspart_service_area_size=syspart_sector0[0x24]+(syspart_sector0[0x25]<<8)+(syspart_sector0[0x26]<<16)+(syspart_sector0[0x27]<<24);
      syspart_service_slot_size=syspart_sector0[0x28]+(syspart_sector0[0x29]<<8)+(syspart_sector0[0x2a]<<16)+(syspart_sector0[0x2b]<<24);
      syspart_service_slot_count=syspart_sector0[0x2c]+(syspart_sector0[0x2d]<<8);
      syspart_service_slotdir_sectors=syspart_sector0[0x2e]+(syspart_sector0[0x2f]<<8);
    }

    if (!partition_start) { retVal=-1; break; }
    if (!partition_size) { retVal=-1; break; }

    // Ok, so we know where the partition starts, so now find the FATs
    if (read_sector(partition_start,fat_mbr,0)) {
      printf("ERROR: Could not read FAT MBR\n");
      retVal=-1; break; }

      if (fat_mbr[510]!=0x55) {
        printf("ERROR: Invalid FAT MBR signature in sector %d ($%x)\n",partition_start,partition_start);
        retVal=-1; break;
      }
      if (fat_mbr[511]!=0xAA) {
        printf("ERROR: Invalid FAT MBR signature in sector %d ($%x)\n",partition_start,partition_start);
        dump_bytes(0,"fat_mbr",fat_mbr,512);
        retVal=-1; break;
      }
      if (fat_mbr[12]!=2) {
        printf("ERROR: FAT32 file system uses a sector size other than 512 bytes\n");
        retVal=-1; break;
      }
      if (fat_mbr[16]!=2) {
        printf("ERROR: FAT32 file system has more or less than 2 FATs\n");
        retVal=-1; break;
      }    
      sectors_per_cluster=fat_mbr[13];
      reserved_sectors=fat_mbr[14]+(fat_mbr[15]<<8);
      data_sectors=(fat_mbr[0x20]<<0)|(fat_mbr[0x21]<<8)|(fat_mbr[0x22]<<16)|(fat_mbr[0x23]<<24);
      sectors_per_fat=(fat_mbr[0x24]<<0)|(fat_mbr[0x25]<<8)|(fat_mbr[0x26]<<16)|(fat_mbr[0x27]<<24);
      first_cluster=(fat_mbr[0x2c]<<0)|(fat_mbr[0x2d]<<8)|(fat_mbr[0x2e]<<16)|(fat_mbr[0x2f]<<24);
      fsinfo_sector=fat_mbr[0x30]+(fat_mbr[0x31]<<8);
      fat1_sector=reserved_sectors;
      fat2_sector=fat1_sector+sectors_per_fat;
      first_cluster_sector=fat2_sector+sectors_per_fat;

      printf("FAT32 file system has %dMB formatted capacity, first cluster = %d, %d sectors per FAT\n",
          data_sectors/2048,first_cluster,sectors_per_fat);
      printf("FATs begin at sector 0x%x and 0x%x\n",fat1_sector,fat2_sector);

      file_system_found=1;

  } while (0);
  return retVal;
}

unsigned char buf[512];

unsigned int get_next_cluster(int cluster)
{
  // printf("get_next_cluster(%d)\n", cluster);
  unsigned int retVal=0xFFFFFFFF;

  do {
    // Read chain entry for this cluster
    int cluster_sector_number=cluster/(512/4);
    int cluster_sector_offset=(cluster*4)&511;

    // Read sector of cluster
    if (read_sector(partition_start+fat1_sector+cluster_sector_number,buf,0)) break;

    // Get value out
    retVal=
      (buf[cluster_sector_offset+0]<<0)|
      (buf[cluster_sector_offset+1]<<8)|
      (buf[cluster_sector_offset+2]<<16)|
      (buf[cluster_sector_offset+3]<<24);

    // mask out highest 4 bits (these seem to be flags on some systems)
    retVal &= 0x0fffffff;

  } while(0);
  return retVal;

}

unsigned char dir_sector_buffer[512];
unsigned int dir_sector=-1; // no dir
int dir_cluster=0;
int dir_sector_in_cluster=0;
int dir_sector_offset=0;

int fat_opendir(char *path)
{
  int retVal=0;
  do {
    if (strcmp(path,"/")) {
      printf("XXX Sub-directories not implemented\n");
    }

    dir_cluster=first_cluster;
    dir_sector=first_cluster_sector;
    dir_sector_offset=-32;
    dir_sector_in_cluster=0;
    retVal=read_sector(partition_start+dir_sector,dir_sector_buffer,0);
    if (retVal) dir_sector=-1;

  } while(0);
  return retVal;
}

int advance_to_next_entry(void)
{
  int retVal = 0;

  // Advance to next entry
  dir_sector_offset+=32;
  if (dir_sector_offset==512) {
    dir_sector_offset=0;
    dir_sector++;
    dir_sector_in_cluster++;
    if (dir_sector_in_cluster==sectors_per_cluster) {
      // Follow to next cluster
      int next_cluster=get_next_cluster(dir_cluster);
      if (next_cluster<0xFFFFFF0&&next_cluster) {
        dir_cluster=next_cluster;
        dir_sector_in_cluster=0;
        dir_sector=first_cluster_sector+(next_cluster-first_cluster)*sectors_per_cluster;
      } else {
        // End of directory reached
        dir_sector=-1;
        retVal=-2;
        return retVal;
      }
    }
    if (dir_sector!=-1) retVal=read_sector(partition_start+dir_sector,dir_sector_buffer,0);
    if (retVal) dir_sector=-1;      
  }    

  return retVal;
}

void debug_vfatchunk(void)
{
  int start = 0x01;
  int len = 5;

  for (int k = start; k < (start+len*2); k+=2)
    printf("%c", dir_sector_buffer[dir_sector_offset+k]);

  start = 0x0E;
  len = 6;

  for (int k = start; k < (start+len*2); k+=2)
    printf("%c", dir_sector_buffer[dir_sector_offset+k]);

  start = 0x1C;
  len = 2;

  for (int k = start; k < (start+len*2); k+=2)
    printf("%c", dir_sector_buffer[dir_sector_offset+k]);

  printf("\n");
}

void copy_to_dnamechunk_from_offset(char* dnamechunk, int offset, int numuc2chars)
{
  for (int k = 0; k < numuc2chars; k++)
  {
    dnamechunk[k] = dir_sector_buffer[dir_sector_offset+offset+k*2];
  }
}

void copy_vfat_chars_into_dname(char* dname, int seqnumber)
{
  // increment char-pointer to the seqnumber string chunk we'll copy across
  dname = dname + 13 * (seqnumber-1);
  copy_to_dnamechunk_from_offset(dname, 0x01, 5);
  dname += 5;
  copy_to_dnamechunk_from_offset(dname, 0x0E, 6);
  dname += 6;
  copy_to_dnamechunk_from_offset(dname, 0x1C, 2);
}

int fat_readdir(struct dirent *d)
{
  int retVal=0;
  int vfatEntry = 0;
  int deletedEntry = 0;

  do {

    retVal = advance_to_next_entry();

    if (retVal == -2) // exiting due to end-of-directory?
    {
      retVal = -1;
      break;
    }

    if (dir_sector==-1) { retVal=-1; break; }
    if (!d) { retVal=-1; break; }

    // printf("Found dirent %d %d %d\n",dir_sector,dir_sector_offset,dir_sector_in_cluster);

    // Read in all FAT32-VFAT entries to extract out long filenames
    if (dir_sector_buffer[dir_sector_offset+0x0B] == 0x0F) {
      vfatEntry = 1;
      int firstTime = 1;
      int seqnumber;
      do
      {
        // printf("seq = 0x%02X\n", dir_sector_buffer[dir_sector_offset+0x00]);
        // debug_vfatchunk();
        int seq = dir_sector_buffer[dir_sector_offset+0x00];

        if (seq == 0xE5)  // if deleted-entry, then ignore
        {
          //printf("deleteentry!\n");
          deletedEntry=1;
        }

        seqnumber = seq & 0x1F;

        // assure there is a null-terminator
        if (firstTime)
        {
          d->d_name[seqnumber*13] = 0;
          firstTime = 0;
        }

        // vfat seqnumbers will be parsed from high to low, each containing up to 13 UCS-2 characters
        copy_vfat_chars_into_dname(d->d_name, seqnumber);
        advance_to_next_entry();

        // if next dirent is not a vfat entry, break out
        if (dir_sector_buffer[dir_sector_offset+0x0B] != 0x0F)
          break;
      } while (seqnumber != 1);
    }

    // ignore any vfat files starting with '.' (such as mac osx '._*' metadata files)
    if (vfatEntry && d->d_name[0] == '.') {
      //printf("._ vfat hide\n");
      d->d_name[0] = 0;
      return 0;
    }

    // ignored deleted vfat entries too (mac osx '._*' files are marked as deleted entries)
    if (deletedEntry)
    {
      d->d_name[0] = 0;
      return 0;
    }
    
    // if the DOS 8.3 entry is a deleted-entry, then ignore
    if (dir_sector_buffer[dir_sector_offset] == 0xE5)
    {
      d->d_name[0] = 0;
      return 0;
    }

    int attrib = dir_sector_buffer[dir_sector_offset+0x0B];

    // if this is the volume-name of the partition, then ignore
    if (attrib == 0x08) {
      d->d_name[0] = 0;
      return 0;
    }

    // if the hidden attribute is turned on, then ignore
    if (attrib & 0x02) {
      d->d_name[0] = 0;
      return 0;
    }
    

    // Put cluster number in d_ino
    d->d_ino=
      (dir_sector_buffer[dir_sector_offset+0x1A]<<0)|
      (dir_sector_buffer[dir_sector_offset+0x1B]<<8)|
      (dir_sector_buffer[dir_sector_offset+0x14]<<16)|
      (dir_sector_buffer[dir_sector_offset+0x15]<<24);

    // if not vfat-longname, then extract out old 8.3 name
    if (!vfatEntry)
    {
      int namelen=0;
      int nt_flags = dir_sector_buffer[dir_sector_offset+0x0C];
      int basename_lowercase = nt_flags & 0x08;
      int extension_lowercase = nt_flags & 0x10;

      // get the 8-byte filename
      if (dir_sector_buffer[dir_sector_offset]) {
        for(int i=0;i<8;i++)
        {
          if (dir_sector_buffer[dir_sector_offset+i])
          {
            int c = dir_sector_buffer[dir_sector_offset+i];
            if (basename_lowercase)
              c = tolower(c);
            d->d_name[namelen++]=c;
          }
        }
        while(namelen&&d->d_name[namelen-1]==' ') namelen--;
      }
      // get the 3-byte extension
      if (dir_sector_buffer[dir_sector_offset+8]&&dir_sector_buffer[dir_sector_offset+8]!=' ') {
        d->d_name[namelen++]='.';
        for(int i=0;i<3;i++)
        {
          if (dir_sector_buffer[dir_sector_offset+8+i])
          {
            int c = dir_sector_buffer[dir_sector_offset+8+i];
            if (extension_lowercase)
              c = tolower(c);
            d->d_name[namelen++]=c;
          }
        }
        while(namelen&&d->d_name[namelen-1]==' ') namelen--;
      }
      d->d_name[namelen]=0;
    }

    if (dirent_raw && d->d_name[0])
      dump_bytes(0,"dirent raw",&dir_sector_buffer[dir_sector_offset],32);

    d->d_off= //  XXX As a hack we put the size here
      (dir_sector_buffer[dir_sector_offset+0x1C]<<0)|
      (dir_sector_buffer[dir_sector_offset+0x1D]<<8)|
      (dir_sector_buffer[dir_sector_offset+0x1E]<<16)|
      (dir_sector_buffer[dir_sector_offset+0x1F]<<24);
    d->d_reclen=dir_sector_buffer[dir_sector_offset+0xb]; // XXX as a hack, we put DOS file attributes here
    if (d->d_reclen&0xC8) d->d_type=DT_UNKNOWN;
    else if (d->d_reclen&0x10) d->d_type=DT_DIR;
    else d->d_type=DT_REG;

  } while(0);
  return retVal;
}

int chain_cluster(unsigned int cluster,unsigned int next_cluster)
{
  int retVal=0;

  do {
    int fat_sector_num=cluster/(512/4);
    int fat_sector_offset=(cluster*4)&0x1FF;
    if (fat_sector_num>=sectors_per_fat) {
      printf("ERROR: cluster number too large.\n");
      retVal=-1; break;
    }

    // Read in the sector of FAT1
    unsigned char fat_sector[512];
    if (read_sector(partition_start+fat1_sector+fat_sector_num,fat_sector,0)) {
      printf("ERROR: Failed to read sector $%x of first FAT\n",fat_sector_num);
      retVal=-1; break;
    }

    //    dump_bytes(0,"FAT sector",fat_sector,512);

    if (0) printf("Marking cluster $%x in use by writing to offset $%x of FAT sector $%x\n",
        cluster,fat_sector_offset,fat_sector_num);

    // Set the bytes for this cluster to $0FFFFF8 to mark end of chain and in use
    fat_sector[fat_sector_offset+0]=(next_cluster>>0)&0xff;
    fat_sector[fat_sector_offset+1]=(next_cluster>>8)&0xff;
    fat_sector[fat_sector_offset+2]=(next_cluster>>16)&0xff;
    fat_sector[fat_sector_offset+3]=(next_cluster>>24)&0x0f;

    if (0) printf("Marking cluster in use in FAT1\n");

    // Write sector back to FAT1
    if (write_sector(partition_start+fat1_sector+fat_sector_num,fat_sector)) {
      printf("ERROR: Failed to write updated FAT sector $%x to FAT1\n",fat_sector_num);
      retVal=-1; break; }

      if (0) printf("Marking cluster in use in FAT2\n");

      // Write sector back to FAT2
      if (write_sector(partition_start+fat2_sector+fat_sector_num,fat_sector)) {
        printf("ERROR: Failed to write updated FAT sector $%x to FAT1\n",fat_sector_num);
        retVal=-1; break; }

        if (0) printf("Done allocating cluster\n");

  } while(0);

  return retVal;
}

int allocate_cluster(unsigned int cluster)
{
  int retVal=0;

  do {
    int fat_sector_num=cluster/(512/4);
    int fat_sector_offset=(cluster*4)&0x1FF;
    if (fat_sector_num>=sectors_per_fat) {
      printf("ERROR: cluster number too large.\n");
      retVal=-1; break;
    }

    // Read in the sector of FAT1
    unsigned char fat_sector[512];
    if (read_sector(partition_start+fat1_sector+fat_sector_num,fat_sector,0)) {
      printf("ERROR: Failed to read sector $%x of first FAT\n",fat_sector_num);
      retVal=-1; break;
    }

    //    dump_bytes(0,"FAT sector",fat_sector,512);

    if (0) printf("Marking cluster $%x in use by writing to offset $%x of FAT sector $%x\n",
        cluster,fat_sector_offset,fat_sector_num);

    // Set the bytes for this cluster to $0FFFFF8 to mark end of chain and in use
    fat_sector[fat_sector_offset+0]=0xf8;
    fat_sector[fat_sector_offset+1]=0xff;
    fat_sector[fat_sector_offset+2]=0xff;
    fat_sector[fat_sector_offset+3]=0x0f;

    if (0) printf("Marking cluster in use in FAT1\n");

    // Write sector back to FAT1
    if (write_sector(partition_start+fat1_sector+fat_sector_num,fat_sector)) {
      printf("ERROR: Failed to write updated FAT sector $%x to FAT1\n",fat_sector_num);
      retVal=-1; break; }

      if (0) printf("Marking cluster in use in FAT2\n");

      // Write sector back to FAT2
      if (write_sector(partition_start+fat2_sector+fat_sector_num,fat_sector)) {
        printf("ERROR: Failed to write updated FAT sector $%x to FAT1\n",fat_sector_num);
        retVal=-1; break; }

        if (0) printf("Done allocating cluster\n");

  } while(0);

  return retVal;
}

unsigned int chained_cluster(unsigned int cluster)
{
  unsigned int retVal=0;

  do {
    int fat_sector_num=cluster/(512/4);
    int fat_sector_offset=(cluster*4)&0x1FF;
    if (fat_sector_num>=sectors_per_fat) {
      printf("ERROR: cluster number too large.\n");
      retVal=-1; break;
    }

    // Read in the sector of FAT1
    unsigned char fat_sector[512];
    if (read_sector(partition_start+fat1_sector+fat_sector_num,fat_sector,0)) {
      printf("ERROR: Failed to read sector $%x of first FAT\n",fat_sector_num);
      retVal=-1; break;
    }

    // Set the bytes for this cluster to $0FFFFF8 to mark end of chain and in use
    retVal=fat_sector[fat_sector_offset+0];
    retVal|=fat_sector[fat_sector_offset+1]<<8;
    retVal|=fat_sector[fat_sector_offset+2]<<16;
    retVal|=fat_sector[fat_sector_offset+3]<<24;

    //    printf("Cluster %d chains to cluster %d ($%x)\n",cluster,retVal,retVal);

  } while(0);

  return retVal;
}


unsigned char fat_sector[512];

unsigned int find_free_cluster(unsigned int first_cluster)
{
  unsigned int cluster=0;

  int retVal=0;

  do {
    int i,o;

    i = first_cluster / (512/4);
    o = (first_cluster % (512/4)) * 4;

    for(;i<sectors_per_fat;i++) {
      // Read FAT sector
      //      printf("Checking FAT sector $%x for free clusters.\n",i);
      if (read_sector(partition_start+fat1_sector+i,fat_sector,0)) {
        printf("ERROR: Failed to read sector $%x of first FAT\n",i);
        retVal=-1; break;
      }

      if (retVal) break;

      // Search for free sectors
      for(;o<512;o+=4) {
        if (!(fat_sector[o]|fat_sector[o+1]|fat_sector[o+2]|fat_sector[o+3]))
        {
          // Found a free cluster.
          cluster = i*(512/4)+(o/4);
          // printf("cluster sector %d, offset %d yields cluster %d\n",i,o,cluster);
          break;
        }
      }
      o=0;

      if (cluster||retVal) break;
    }

    // printf("I believe cluster $%x is free.\n",cluster);

    retVal=cluster;
  } while(0);

  return retVal;
}

int show_directory(char *path)
{
  struct dirent de;
  int retVal=0;
  do {
    if (!file_system_found) open_file_system();
    if (!file_system_found) {
      fprintf(stderr,"ERROR: Could not open file system.\n");
      retVal=-1;
      break;
    }

    if (fat_opendir(path)) { retVal=-1; break; }
    // printf("Opened directory, dir_sector=%d (absolute sector = %d)\n",dir_sector,partition_start+dir_sector);
    while(!fat_readdir(&de)) {
      if (de.d_name[0])
      {
        if (de.d_type == DT_DIR)
          printf("%12s %s\n", "<dir>",de.d_name);
        else
          printf("%12d %s\n",(int)de.d_off,de.d_name);
      }
    }
  } while(0);

  return retVal;
}

int read_int32_from_offset_in_buffer(int offset)
{
  int val =
    (dir_sector_buffer[offset]<<0)
    |(dir_sector_buffer[offset+1]<<8)
    |(dir_sector_buffer[offset+2]<<16)
    |(dir_sector_buffer[offset+3]<<24);

  return val;
}

void show_clustermap(void)
{
  int clustermap_end = clustermap_start + clustermap_count;
  int previous_clustermap_sector = 0;
  int abs_fat1_sector = partition_start + fat1_sector;

  for (int clustermap_idx = clustermap_start; clustermap_idx < clustermap_end; clustermap_idx++)
  {
    int clustermap_sector = abs_fat1_sector + (clustermap_idx*4) / 512;
    int clustermap_offset = (clustermap_idx*4) % 512;

    //printf("clustermap_sector = %d\nclustermap_offset=%d\n", clustermap_sector, clustermap_offset);

    // do we need to read in the next sector?
    if (clustermap_sector != previous_clustermap_sector)
    {
      int retVal=read_sector(clustermap_sector,dir_sector_buffer,0);
      if (retVal)
      {
        fprintf(stderr, "Failed to read next sector(%d)\n", clustermap_sector);
        return;
      }
      previous_clustermap_sector = clustermap_sector;
    }

    int clustermap_val = read_int32_from_offset_in_buffer(clustermap_offset);
    clustermap_val &= 0x0fffffff; // map out flags in top 4 bits
    printf("%d:  %d  ($%08X)\n", clustermap_idx, clustermap_val, clustermap_val);
  }
}

void show_cluster(void)
{
  char str[50];
  int abs_cluster2_sector = partition_start + first_cluster_sector + (cluster_num-2)*sectors_per_cluster;

  for (int idx = 0; idx < sectors_per_cluster; idx++)
  {
    read_sector(abs_cluster2_sector + idx, dir_sector_buffer, 0);
    sprintf(str, "Sector %d:\n", abs_cluster2_sector + idx);
    dump_bytes(0,str,dir_sector_buffer,512);
  }
}

int rename_file(char *name,char *dest_name)
{
  struct dirent de;
  int retVal=0;
  do {

    if (!file_system_found) open_file_system();
    if (!file_system_found) {
      fprintf(stderr,"ERROR: Could not open file system.\n");
      retVal=-1;
      break;
    }

    if (fat_opendir("/")) { retVal=-1; break; }
    // printf("Opened directory, dir_sector=%d (absolute sector = %d)\n",dir_sector,partition_start+dir_sector);
    while(!fat_readdir(&de)) {
      // if (de.d_name[0]) printf("'%s'   %d\n",de.d_name,(int)de.d_off);
      // else dump_bytes(0,"empty dirent",&dir_sector_buffer[dir_sector_offset],32);
      if (!strcasecmp(de.d_name,name)) {
        // Found file, so will replace it
        printf("%s already exists on the file system, beginning at cluster %d\n",name,(int)de.d_ino);
        break;
      }
    }
    if (dir_sector==-1) {
      printf("File %s does not exist.\n",name);
      retVal=-1; break;
    }

    // Write name
    for(int i=0;i<11;i++) dir_sector_buffer[dir_sector_offset+i]=0x20;
    for(int i=0;i<9;i++)
      if (dest_name[i]=='.') {
        // Write out extension
        for(int j=0;j<3;j++)
          if (dest_name[i+1+j]) dir_sector_buffer[dir_sector_offset+8+j]=dest_name[i+1+j];
        break;
      } else if (!dest_name[i]) break;
      else dir_sector_buffer[dir_sector_offset+i]=dest_name[i];

    // Write modified directory entry back to disk
    if (write_sector(partition_start+dir_sector,dir_sector_buffer)) {
      printf("Failed to write updated directory sector.\n");
      retVal=-1; break; }

  } while(0);

  return retVal;
}


int upload_file(char *name,char *dest_name)
{
  struct dirent de;
  int retVal=0;
  do {

    time_t upload_start=time(0);

    struct stat st;
    if (stat(name,&st)) {
      fprintf(stderr,"ERROR: Could not stat file '%s'\n",name);
      perror("stat() failed");
    }
    //    printf("File '%s' is %ld bytes long.\n",name,(long)st.st_size);

    if (!file_system_found) open_file_system();
    if (!file_system_found) {
      fprintf(stderr,"ERROR: Could not open file system.\n");
      retVal=-1;
      break;
    }

    if (fat_opendir("/")) { retVal=-1; break; }
    //    printf("Opened directory, dir_sector=%d (absolute sector = %d)\n",dir_sector,partition_start+dir_sector);
    while(!fat_readdir(&de)) {
      // if (de.d_name[0]) printf("%13s   %d\n",de.d_name,(int)de.d_off);
      //      else dump_bytes(0,"empty dirent",&dir_sector_buffer[dir_sector_offset],32);
      if (!strcasecmp(de.d_name,dest_name)) {
        // Found file, so will replace it
        //	printf("%s already exists on the file system, beginning at cluster %d\n",name,(int)de.d_ino);
        break;
      }
    }
    if (dir_sector==-1) {
      // File does not (yet) exist, get ready to create it
      printf("%s does not yet exist on the file system -- searching for empty directory slot to create it in.\n",name);

      if (fat_opendir("/")) { retVal=-1; break; }
      struct dirent de;
      while(!fat_readdir(&de)) {
        if (!de.d_name[0]) {
          if (0) printf("Found empty slot at dir_sector=%d, dir_sector_offset=%d\n",
              dir_sector,dir_sector_offset);

          // Create directory entry, and write sector back to SD card
          unsigned char dir[32];
          bzero(dir,32);

          // Write name
          for(int i=0;i<11;i++) dir[i]=0x20;
          for(int i=0;i<9;i++)
            if (dest_name[i]=='.') {
              // Write out extension
              for(int j=0;j<3;j++)
                if (dest_name[i+1+j]) dir[8+j]=dest_name[i+1+j];
              break;
            } else if (!dest_name[i]) break;
            else dir[i]=dest_name[i];

          // Set file attributes (only archive bit)
          dir[0xb]=0x20;

          // Store create time and date
          time_t t=time(0);
          struct tm *tm=localtime(&t);
          dir[0xe]=(tm->tm_sec>>1)&0x1F;  // 2 second resolution
          dir[0xe]|=(tm->tm_min&0x7)<<5;
          dir[0xf]=(tm->tm_min&0x3)>>3;
          dir[0xf]|=(tm->tm_hour)<<2;
          dir[0x10]=tm->tm_mday&0x1f;
          dir[0x10]|=((tm->tm_mon+1)&0x7)<<5;
          dir[0x11]=((tm->tm_mon+1)&0x1)>>3;
          dir[0x11]|=(tm->tm_year-80)<<1;

          //	  dump_bytes(0,"New directory entry",dir,32);

          // (Cluster and size we set after writing to the file)

          // Copy back into directory sector, and write it
          bcopy(dir,&dir_sector_buffer[dir_sector_offset],32);
          if (write_sector(partition_start+dir_sector,dir_sector_buffer)) {
            printf("Failed to write updated directory sector.\n");
            retVal=-1; break; }

            break;
        }
      }
    }
    if (dir_sector==-1) {
      printf("ERROR: Directory is full.  Request support for extending directory into multiple clusters.\n");
      retVal=-1;
      break;
    } else {
      //      printf("Directory entry is at offset $%03x of sector $%x\n",dir_sector_offset,dir_sector);
    }

    // Read out the first cluster. If zero, then we need to allocate a first cluster.
    // After that, we can allocate and chain clusters in a constant manner
    unsigned int first_cluster_of_file=
      (dir_sector_buffer[dir_sector_offset+0x1A]<<0)
      |(dir_sector_buffer[dir_sector_offset+0x1B]<<8)
      |(dir_sector_buffer[dir_sector_offset+0x14]<<16)
      |(dir_sector_buffer[dir_sector_offset+0x15]<<24);
    if (!first_cluster_of_file) {
      //      printf("File currently has no first cluster allocated.\n");

      int a_cluster=find_free_cluster(0);
      if (!a_cluster) {
        printf("ERROR: Failed to find a free cluster.\n");
        retVal=-1; break;
      }
      if (allocate_cluster(a_cluster)) {
        printf("ERROR: Could not allocate cluster $%x\n",a_cluster);
        retVal=-1; break;	
      }

      // Write cluster number into directory entry
      dir_sector_buffer[dir_sector_offset+0x1A]=(a_cluster>>0)&0xff;
      dir_sector_buffer[dir_sector_offset+0x1B]=(a_cluster>>8)&0xff;
      dir_sector_buffer[dir_sector_offset+0x14]=(a_cluster>>16)&0xff;
      dir_sector_buffer[dir_sector_offset+0x15]=(a_cluster>>24)&0xff;

      if (write_sector(partition_start+dir_sector,dir_sector_buffer)) {
        printf("ERROR: Failed to write updated directory sector after allocating first cluster.\n");
        retVal=-1; break; }

        first_cluster_of_file=a_cluster;
    } // else printf("First cluster of file is $%x\n",first_cluster_of_file);

    // Now write the file out sector by sector, and allocate new clusters as required
    int remaining_length=st.st_size;
    int sector_in_cluster=0;
    int file_cluster=first_cluster_of_file;
    unsigned int sector_number;
    FILE *f=fopen(name,"r");

    if (!f) {
      printf("ERROR: Could not open file '%s' for reading.\n",name);
      retVal=-1; break;
    }

    while(remaining_length>0) {
      if (sector_in_cluster>=sectors_per_cluster) {
        // Advance to next cluster
        // If we are currently the last cluster, then allocate a new one, and chain it in

        int next_cluster=chained_cluster(file_cluster);
        if (next_cluster==0||next_cluster>=0xffffff8) {
          next_cluster=find_free_cluster(file_cluster);
          if (allocate_cluster(next_cluster)) {
            printf("ERROR: Could not allocate cluster $%x\n",next_cluster);
            retVal=-1; break;
          }
          if (chain_cluster(file_cluster,next_cluster)) {
            printf("ERROR: Could not chain cluster $%x to $%x\n",file_cluster,next_cluster);
            retVal=-1; break;
          }
        }
        if (!next_cluster) {
          printf("ERROR: Could not find a free cluster\n");
          retVal=-1; break;
        }


        file_cluster=next_cluster;
        sector_in_cluster=0;
      }

      // Write sector
      unsigned char buffer[512];
      bzero(buffer,512);
      int bytes=fread(buffer,1,512,f);
      sector_number=partition_start+first_cluster_sector+(sectors_per_cluster*(file_cluster-first_cluster))+sector_in_cluster;
      if (0) printf("T+%lld : Read %d bytes from file, writing to sector $%x (%d) for cluster %d\n",
          gettime_us()-start_usec,bytes,sector_number,sector_number,file_cluster);
      printf("\rUploaded %lld bytes.",(long long)st.st_size-remaining_length);
      fflush(stdout);

      if (write_sector(sector_number,buffer)) {
        printf("ERROR: Failed to write to sector %d\n",sector_number);
        retVal=-1;
        break;
      }
      //      printf("T+%lld : after write.\n",gettime_us()-start_usec);

      sector_in_cluster++;
      remaining_length-=512;
    }

    // XXX check for orphan clusters at the end, and if present, free them.

    // Write file size into directory entry
    dir_sector_buffer[dir_sector_offset+0x1C]=(st.st_size>>0)&0xff;
    dir_sector_buffer[dir_sector_offset+0x1D]=(st.st_size>>8)&0xff;
    dir_sector_buffer[dir_sector_offset+0x1E]=(st.st_size>>16)&0xff;
    dir_sector_buffer[dir_sector_offset+0x1F]=(st.st_size>>24)&0xff;

    if (write_sector(partition_start+dir_sector,dir_sector_buffer)) {
      printf("ERROR: Failed to write updated directory sector after updating file length.\n");
      retVal=-1; break; }

      // Flush any pending sector writes out
      execute_write_queue();

      if (time(0)==upload_start) upload_start=time(0)-1;
      printf("\rUploaded %lld bytes in %lld seconds (%.1fKB/sec)\n",
          (long long)st.st_size,(long long)time(0)-upload_start,st.st_size*1.0/1024/(time(0)-upload_start));

  } while(0);

  return retVal;
}

unsigned char download_buffer[512];

int download_slot(int slot_number,char *dest_name)
{
  int retVal=0;
  do {

    if (!syspart_start) {
      printf("ERROR: No system partition detected.\n");
      retVal=-1;
      break;
    }

    if (slot_number<0||slot_number>=syspart_slot_count) {
      printf("ERROR: Invalid slot number (valid range is 0 -- %d)\n",syspart_slot_count);
      retVal=-1;
      break;
    }

    FILE *f=fopen(dest_name,"w");
    if (!f) {
      printf("ERROR: Could not open file '%s' for writing\n",dest_name);
      retVal=-1;
      break;
    }
    printf("Saving slot %d into '%s'\n",slot_number,dest_name);

    for(int i=0;i<syspart_slot_size;i++) {
      unsigned char sector[512];
      int sector_num=syspart_start+syspart_freeze_area+syspart_slotdir_sectors+slot_number*syspart_slot_size+i;
      if (!i) printf("Downloading %d sectors beginning at sector $%08x\n",
          syspart_slot_size,sector_num);
      if (read_sector(sector_num,sector,0))
      {
        printf("ERROR: Could not read sector %d/%d of freeze slot %d (absolute sector %d)\n",
            i,syspart_slot_size,slot_number,sector_num);
        retVal=-1;
        break;
      }
      fwrite(sector,512,1,f);
      printf("."); fflush(stdout);
    }
    fclose(f);
    printf("\n");

  } while(0);

  return retVal;
}

int download_file(char *dest_name,char *local_name,int showClusters)
{
  struct dirent de;
  int retVal=0;
  do {

    time_t upload_start=time(0);

    if (!file_system_found) open_file_system();
    if (!file_system_found) {
      fprintf(stderr,"ERROR: Could not open file system.\n");
      retVal=-1;
      break;
    }

    if (fat_opendir("/")) { retVal=-1; break; }
    //    printf("Opened directory, dir_sector=%d (absolute sector = %d)\n",dir_sector,partition_start+dir_sector);
    while(!fat_readdir(&de)) {
      // if (de.d_name[0]) printf("%13s   %d\n",de.d_name,(int)de.d_off);
      //      else dump_bytes(0,"empty dirent",&dir_sector_buffer[dir_sector_offset],32);
      if (!strcasecmp(de.d_name,dest_name)) {
        // Found file, so will replace it
        //	printf("%s already exists on the file system, beginning at cluster %d\n",name,(int)de.d_ino);
        break;
      }
    }
    if (dir_sector==-1) {
      printf("?  FILE NOT FOUND ERROR FOR \"%s\"\n",dest_name);
      retVal=-1; break; 
    }

    // Read out the first cluster. If zero, then we need to allocate a first cluster.
    // After that, we can allocate and chain clusters in a constant manner
    unsigned int first_cluster_of_file=
      (dir_sector_buffer[dir_sector_offset+0x1A]<<0)
      |(dir_sector_buffer[dir_sector_offset+0x1B]<<8)
      |(dir_sector_buffer[dir_sector_offset+0x14]<<16)
      |(dir_sector_buffer[dir_sector_offset+0x15]<<24);

    // Now write the file out sector by sector, and allocate new clusters as required
    int remaining_bytes=de.d_off;
    int sector_in_cluster=0;
    int file_cluster=first_cluster_of_file;
    unsigned int sector_number;
    FILE *f=NULL;

    if (!showClusters) {
      f=fopen(local_name,"w");
      if (!f) {
        printf("ERROR: Could not open file '%s' for writing.\n",local_name);
        retVal=-1; break;
      }
    } else printf("Clusters: %d",file_cluster);

    while(remaining_bytes) {
      if (sector_in_cluster>=sectors_per_cluster) {
        // Advance to next cluster
        // If we are currently the last cluster, then allocate a new one, and chain it in

        int next_cluster=chained_cluster(file_cluster);	
        if (next_cluster==0||next_cluster>=0xffffff8) {
          printf("\n?  PREMATURE END OF FILE ERROR\n");
          if (f) fclose(f);
          retVal=-1; break;
        }
        if (showClusters) {
          if (next_cluster==(file_cluster+1)) printf("."); else printf("%d, %d",file_cluster,next_cluster);
        }

        file_cluster=next_cluster;
        sector_in_cluster=0;
      }

      if (f) {
        // Read sector
        sector_number=partition_start+first_cluster_sector+(sectors_per_cluster*(file_cluster-first_cluster))+sector_in_cluster;

        if (read_sector(sector_number,download_buffer,0)) {
          printf("ERROR: Failed to read to sector %d\n",sector_number);
          retVal=-1;
          if (f) fclose(f);
          break;
        }

        if (remaining_bytes>=512)
          fwrite(download_buffer,512,1,f);
        else
          fwrite(download_buffer,remaining_bytes,1,f);
      }

      if (0) printf("T+%lld : Read %d bytes from file, writing to sector $%x (%d) for cluster %d\n",
          gettime_us()-start_usec,(int)de.d_off,sector_number,sector_number,file_cluster);
      if (!showClusters) printf("\rDownloaded %lld bytes.",(long long)de.d_off-remaining_bytes);
      fflush(stdout);

      //      printf("T+%lld : after write.\n",gettime_us()-start_usec);

      sector_in_cluster++;
      remaining_bytes-=512;
    }

    if (f) fclose(f);

    if (time(0)==upload_start) upload_start=time(0)-1;
    if (!showClusters) {
      printf("\rDownloaded %lld bytes in %lld seconds (%.1fKB/sec)\n",
          (long long)de.d_off,(long long)time(0)-upload_start,de.d_off*1.0/1024/(time(0)-upload_start));
    } else printf("\n");

  } while(0);

  return retVal;
}
