//****************************************************************************
// Copyright (C) 2001-2006  PEAK System-Technik GmbH
//
// linux@peak-system.com 
// www.peak-system.com
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
//
// Maintainer(s): Klaus Hitschler (klaus.hitschler@gmx.de)
//****************************************************************************

//****************************************************************************
//
// transmitest.cpp - a simple program to test CAN transmits
//
// $Id: transmitest.cpp 499 2007-04-25 09:18:10Z edouard $
//
//****************************************************************************

//----------------------------------------------------------------------------
// set here current release for this program
#define CURRENT_RELEASE "Release_20060501_a"  

//****************************************************************************
// INCLUDES
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <ctype.h>
#include <unistd.h>   // exit
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>    // O_RDWR

#include <libpcan.h>
#include <src/common.h>
#include <src/parser.h>

#ifdef XENOMAI
#include <sys/mman.h>
#include <native/task.h>
#include <native/timer.h>
#endif

//****************************************************************************
// DEFINES
#ifdef XENOMAI
#define STATE_FILE_OPENED         1
#define STATE_TASK_CREATED        2
#endif
#define DEFAULT_NODE "/dev/pcan0"

//****************************************************************************
// GLOBALS
#ifdef XENOMAI
unsigned int current_state = 0;
int shutdownnow = 0;
std::list<TPCANMsg> *List;
int nExtended = CAN_INIT_TYPE_ST;
RT_TASK writing_task;
#endif
HANDLE h = NULL;
char *current_release;

//****************************************************************************
// LOCALS

//****************************************************************************
// CODE 
static void hlpMsg(void)
{
  printf("transmitest - a small test program which sends CAN messages.\n");
  printf("usage:   transmitest filename {[-f=devicenode] | {[-t=type] [-p=port [-i=irq]]}} [-b=BTR0BTR1] [-e] [-?]\n");
  printf("options: filename - mandatory name of message description file.\n");
  printf("         -f - devicenode - path to devicefile, default=%s\n", DEFAULT_NODE);
  printf("         -t - type of interface, e.g. 'pci', 'sp', 'epp' ,'isa', 'pccard' or 'usb' (default: pci).\n");
  printf("         -p - port in hex notation if applicable, e.g. 0x378 (default: 1st port of type).\n");
  printf("         -i - irq in dec notation if applicable, e.g. 7 (default: irq of 1st port).\n");
  printf("         -b - BTR0BTR1 code in hex, e.g. 0x001C (default: 500 kbit).\n");
  printf("         -e - accept extended frames. (default: standard frames)\n");
  printf("         -? or --help - this help\n");
  printf("\n");
}

#ifdef XENOMAI

//----------------------------------------------------------------------------
// cleaning up
void cleanup_all(void)
{
  if (current_state & STATE_FILE_OPENED)
  {
    print_diag("transmitest");
    CAN_Close(h);
    current_state &= ~STATE_FILE_OPENED;
  }
  if (current_state & STATE_TASK_CREATED)
  {
    rt_task_delete(&writing_task);
    current_state &= ~STATE_TASK_CREATED;
  }
}

void catch_signal(int sig)
{
  signal(SIGTERM, catch_signal);
  signal(SIGINT, catch_signal);
  shutdownnow = 1;
  cleanup_all();
  printf("transmitest: finished(0)\n");
}

//----------------------------------------------------------------------------
// real time task
void writing_task_proc(void *arg)
{
  while (1)
  {
    std::list<TPCANMsg>::iterator iter;

    for (iter = List->begin(); iter != List->end(); iter++)
    {
      // test for standard frames only
      if ((nExtended == CAN_INIT_TYPE_EX) || !(iter->MSGTYPE & MSGTYPE_EXTENDED))
      {
        // send the message
        if ((errno = CAN_Write(h, &(*iter))))
	  shutdownnow = 1;
      }
      if (shutdownnow == 1) break;
    }
    if (shutdownnow == 1) break;
  }
}

//----------------------------------------------------------------------------
// all is done here
int main(int argc, char *argv[])
{
  char *ptr;
  int i;
  int nType = HW_PCI;
  __u32 dwPort = 0;
  __u16 wIrq = 0;
  __u16 wBTR0BTR1 = 0;
  char  *filename = NULL;
  char  *szDevNode = DEFAULT_NODE;
  bool bDevNodeGiven = false;
  bool bTypeGiven = false;
  parser MyParser;
  char txt[VERSIONSTRING_LEN];

  mlockall(MCL_CURRENT | MCL_FUTURE);

  errno = 0;

  current_release = CURRENT_RELEASE;
  disclaimer("transmitest");

  // decode command line arguments
  for (i = 1; i < argc; i++)
  {
    char c;

    ptr = argv[i];

    if (*ptr == '-')
    {
      while (*ptr == '-')
        ptr++;

      c = *ptr;
      ptr++;

      if (*ptr == '=')
      ptr++;

      switch(tolower(c))
      {
        case 'f':
          szDevNode = ptr;
          bDevNodeGiven = true;
          break;
        case 't':
          nType = getTypeOfInterface(ptr);
          if (!nType)
          {
            errno = EINVAL;
            printf("transmitest: unknown type of interface\n");
            goto error;
          }
          bTypeGiven = true;
          break;
        case 'p':
          dwPort = strtoul(ptr, NULL, 16);
          break;
        case 'i':
          wIrq   = (__u16)strtoul(ptr, NULL, 10);
          break;
        case 'e':
          nExtended = CAN_INIT_TYPE_EX;
          break;
        case '?': 
        case 'h':
          hlpMsg();
          errno = 0;
          goto error;
          break;
        case 'b':
          wBTR0BTR1 = (__u16)strtoul(ptr, NULL, 16);
          break;
        default:
          errno = EINVAL;
          printf("transmitest: unknown command line argument\n");
          goto error;
          break;
      }
    }
    else
      filename = ptr;
  }

  // test for filename
  if (filename == NULL)
  {
    errno = EINVAL;
    perror("transmitest: no filename given");
    goto error;
  }

  if (bDevNodeGiven && bTypeGiven)
  {
    errno = EINVAL;
    perror("transmitest: device node and type together is useless");
    goto error;
  }

  // give the filename to my parser
  MyParser.setFileName(filename);

  // tell some information to user
  if (!bTypeGiven)
  {
    printf("transmitest: device node=\"%s\"\n", szDevNode);
  }
  else
  {
    printf("transmitest: type=%s", getNameOfInterface(nType));
    if (nType == HW_USB)
    {
      printf(", Serial Number=default, Device Number=%d\n", dwPort); 
    }
    else
    {
      if (dwPort)
      {
        if (nType == HW_PCI)
          printf(", %d. PCI device", dwPort);
        else
          printf(", port=0x%08x", dwPort);
      }
      else
        printf(", port=default");
      if ((wIrq) && !(nType == HW_PCI))
        printf(" irq=0x%04x\n", wIrq);
      else
        printf(", irq=default\n");
    }
  }

  if (nExtended == CAN_INIT_TYPE_EX)
    printf("             Extended frames are sent");
  else
    printf("             Only standard frames are sent");
  if (wBTR0BTR1)
    printf(", init with BTR0BTR1=0x%04x\n", wBTR0BTR1);
  else
    printf(", init with 500 kbit/sec.\n");

   printf("             Data will be read from \"%s\".\n", filename);

  /* install signal handler for manual break */
  signal(SIGTERM, catch_signal);
  signal(SIGINT, catch_signal);

  /* start timer */
  errno = rt_timer_set_mode(TM_ONESHOT);
  if (errno)
  {
    printf("transmitest: error while configuring timer\n");
    goto error;
  }

  /* get the list of data from parser */
  List = MyParser.Messages();
  if (!List)
  {
    errno = MyParser.nGetLastError();
    perror("transmitest: error at file read");
    goto error;
  }

  /* open the CAN port */
  if ((bDevNodeGiven) || (!bDevNodeGiven && !bTypeGiven))
  {
    h = LINUX_CAN_Open(szDevNode, O_RDWR);
    if (h)
      current_state |= STATE_FILE_OPENED;
    else
    {
      printf("transmitest: can't open %s\n", szDevNode);
      goto error;
    }
  }
  else
  {
    // please use what is appropriate  
    // HW_DONGLE_SJA 
    // HW_DONGLE_SJA_EPP 
    // HW_ISA_SJA 
    // HW_PCI 
    h = CAN_Open(nType, dwPort, wIrq);
    if (h)
      current_state |= STATE_FILE_OPENED;
    else
    {
      printf("transmitest: can't open %s device.\n", getNameOfInterface(nType));
      goto error;
    }
  }

  /* clear status */
  CAN_Status(h);

  /* get version info */
  errno = CAN_VersionInfo(h, txt);
  if (!errno)
    printf("transmitest: driver version = %s\n", txt);
  else
  {
    printf("transmitest: Error while getting version info\n");
    goto error;
  }

  /* init to a user defined bit rate */
  if (wBTR0BTR1)
  {
    errno = CAN_Init(h, wBTR0BTR1, nExtended);
    if (errno)
    {
      printf("transmitest: Error while initializing CAN device\n");
      goto error;
    }
  }

  //create writing_task
  errno = rt_task_create(&writing_task,NULL,0,50,0);
  if (errno) {
    printf("transmitest: Failed to create rt task, code %d\n",errno);
    goto error;
  }
  current_state |= STATE_TASK_CREATED;

  printf("transmitest: writing data to CAN ... (press Ctrl-C to exit)\n");

  // start writing_task
  errno = rt_task_start(&writing_task,&writing_task_proc,NULL);
  if (errno) {
    printf("transmitest: Failed to start rt task, code %d\n",errno);
    goto error;
  }

  pause();
  return 0;

error:
  cleanup_all();
  return errno;
}

#else

//----------------------------------------------------------------------------
// centralized entry point for all exits 
static void my_private_exit(int error)
{
  if (h)
  {
    print_diag("transmitest");
    CAN_Close(h); 
  }
  printf("transmitest: finished (%d).\n\n", error);
  exit(error);
}

//----------------------------------------------------------------------------
// handles CTRL-C user interrupt
static void signal_handler(int signal)
{
  my_private_exit(0);
} 

//----------------------------------------------------------------------------
// all is done here
int main(int argc, char *argv[])
{ 
  char *ptr;
  int i;
  int nType = HW_PCI;
  __u32 dwPort = 0;
  __u16 wIrq = 0;
  __u16 wBTR0BTR1 = 0;
  int   nExtended = CAN_INIT_TYPE_ST;
  char  *filename = NULL;
	char  *szDevNode = DEFAULT_NODE;
	bool bDevNodeGiven = false;
	bool bTypeGiven = false;
  int   counter = 0;
  parser MyParser;
  std::list<TPCANMsg> *List;
  
  errno = 0;
  
  current_release = CURRENT_RELEASE;
  disclaimer("transmitest");

  // decode command line arguments
  for (i = 1; i < argc; i++)
  {
    char c;
    
    ptr = argv[i];
    
    if (*ptr == '-')
    {
      while (*ptr == '-')
        ptr++;
	
      c = *ptr;
      ptr++;

      if (*ptr == '=')
      ptr++;

      switch(tolower(c))
      {
				case 'f':
					szDevNode = ptr;
					bDevNodeGiven = true;
					break;
        case 't':
          nType = getTypeOfInterface(ptr);
	        if (!nType)
	        {
		        errno = EINVAL;
		        printf("transmitest: unknown type of interface");
		        my_private_exit(errno);
	        }
				  bTypeGiven = true;
          break;
        case 'p':
	        dwPort = strtoul(ptr, NULL, 16);
	        break;
        case 'i':
	        wIrq   = (__u16)strtoul(ptr, NULL, 10);
	        break;
        case 'e':
	        nExtended = CAN_INIT_TYPE_EX;
	        break;
        case '?': 
        case 'h':
          hlpMsg();
	        my_private_exit(0);
	        break;
        case 'b':
	        wBTR0BTR1 = (__u16)strtoul(ptr, NULL, 16);
	        break;
        default:
	        errno = EINVAL;
	        printf("transmitest: unknown command line argument");
	        my_private_exit(errno);
	        break;
      }
    }
    else
      filename = ptr;            
  }

  // test for filename
  if (filename == NULL)
  {
    errno = EINVAL;
    perror("transmitest: no filename given");
    my_private_exit(errno);
  }
	
	if (bDevNodeGiven && bTypeGiven)
	{
    errno = EINVAL;
    perror("transmitest: device node and type together is useless");
    my_private_exit(errno);
	}
	
  // give the filename to my parser
  MyParser.setFileName(filename);
    
  // tell some information to user
	if (!bTypeGiven)
	{
		printf("transmitest: device node=\"%s\"\n", szDevNode);
	}
	else
	{
		printf("transmitest: type=%s", getNameOfInterface(nType));
		if (nType == HW_USB)
		{
			printf(", Serial Number=default, Device Number=%d\n", dwPort); 
		}
		else
		{
			if (dwPort)
			{
				if (nType == HW_PCI)
					printf(", %d. PCI device", dwPort);
				else
					printf(", port=0x%08x", dwPort);
			}
			else
				printf(", port=default");
			if ((wIrq) && !(nType == HW_PCI))
				printf(" irq=0x%04x\n", wIrq);
			else
				printf(", irq=default\n");
		}
	}
    
  if (nExtended == CAN_INIT_TYPE_EX)
    printf("             Extended frames are sent");
  else
    printf("             Only standard frames are sent");
  if (wBTR0BTR1)
    printf(", init with BTR0BTR1=0x%04x\n", wBTR0BTR1);
  else
    printf(", init with 500 kbit/sec.\n");
    
  printf("             Data will be read from \"%s\".\n", filename);
    
  
  // install signal handler for manual break
  signal(SIGINT, signal_handler);
  
  // get the list of data from parser
  List = MyParser.Messages();
  if (!List)
  {
    errno = MyParser.nGetLastError();
    perror("transmitest: error at file read");
    my_private_exit(errno);    
  }
 
	// open the CAN port
	if ((bDevNodeGiven) || (!bDevNodeGiven && !bTypeGiven))
	{
    h = LINUX_CAN_Open(szDevNode, O_RDWR);  
	}
	else
	{
		// please use what is appropriate  
		// HW_DONGLE_SJA 
		// HW_DONGLE_SJA_EPP 
		// HW_ISA_SJA 
		// HW_PCI 
		h = CAN_Open(nType, dwPort, wIrq);
	}
  
  if (h)
  {
    char txt[VERSIONSTRING_LEN];
    int  err;
    
    // clear status
    CAN_Status(h);
   
    // get version info
    errno = CAN_VersionInfo(h, txt);
    if (!errno)
      printf("transmitest: driver version = %s\n", txt);
    else
    {
      perror("transmitest: CAN_VersionInfo()");
      my_private_exit(errno);
    }
      
    // init to a user defined bit rate
    if (wBTR0BTR1)
    {
      errno = CAN_Init(h, wBTR0BTR1, nExtended);
      if (errno)
      {
        perror("transmitest: CAN_Init()");
	      my_private_exit(errno);
	    }
	  }
	  
	  printf("transmitest: writing data to CAN ... (press Ctrl-C to exit)\n");
      
    // write out endless loop until Ctrl-C
    while (1)
    {
      std::list<TPCANMsg>::iterator iter;
      int i;
    
      for (iter = List->begin(); iter != List->end(); iter++)
      {  
        // test for standard frames only
        if ((nExtended == CAN_INIT_TYPE_EX) || !(iter->MSGTYPE & MSGTYPE_EXTENDED))
	      {  
	        // send the message
          if ((errno = CAN_Write(h, &(*iter))))
          {
            perror("transmitest: CAN_Write()");
	          my_private_exit(errno);
	        }
	      }
		  }    
    }
  }
  else
  {
    errno = nGetLastError();
    perror("transmitest: CAN_Open() or LINUX_CAN_Open()");
    my_private_exit(errno);
  }
    
  return errno;
}
 

#endif

