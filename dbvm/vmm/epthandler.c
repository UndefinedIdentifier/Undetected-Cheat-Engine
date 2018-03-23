/*
 * epthandler.c
 *
 *  Created on: Feb 2, 2018
 *      Author: erich
 */

#include "epthandler.h"
#include "main.h"
#include "mm.h"
#include "vmxsetup.h"
#include "msrnames.h"
#include "common.h"
#include "vmpaging.h"
#include "vmcall.h"

QWORD EPTMapPhysicalMemory(pcpuinfo currentcpuinfo, QWORD physicalAddress, int forcesmallpage);

criticalSection eptWatchListCS;
PEPTWatchEntry eptWatchList;
int eptWatchListSize;
int eptWatchListPos;

criticalSection CloakedPagesCS; //1
CloakedPageInfo *CloakedPages;
int CloakedPagesSize;
int CloakedPagesPos;

criticalSection ChangeRegBPListCS; //2
ChangeRegBPEntry *ChangeRegBPList;
int ChangeRegBPListSize;
int ChangeRegBPListPos;

BOOL ept_handleCloakEvent(pcpuinfo currentcpuinfo, QWORD Address)
/*
 * Checks if the physical address is cloaked, if so handle it and return 1, else return 0
 */
{
  int i,result=0;
  QWORD BaseAddress=Address & MAXPHYADDRMASKPB;

  if (CloakedPagesPos==0)
    return FALSE;

  csEnter(&CloakedPagesCS);
  for (i=0; i<CloakedPagesPos; i++)
  {
    if (currentcpuinfo->eptCloakListLength<=i)
      break;

    csEnter(&currentcpuinfo->EPTPML4CS);

    if (CloakedPages[i].PhysicalAddressExecutable==BaseAddress)
    {
      //it's a cloaked page
      EPT_VIOLATION_INFO evi;
      evi.ExitQualification=vmread(vm_exit_qualification);

      if (evi.X) //looks like this cpu does not support execute only
      {
        *(QWORD *)(currentcpuinfo->eptCloakList[i])=CloakedPages[i].PhysicalAddressExecutable;
        currentcpuinfo->eptCloakList[i]->WA=1;
        currentcpuinfo->eptCloakList[i]->RA=1;
        currentcpuinfo->eptCloakList[i]->XA=1;

        vmx_enableSingleStepMode();
        vmx_addSingleSteppingReason(currentcpuinfo, 2,i);
        currentcpuinfo->eptCloak_LastOperationWasWrite=evi.W;

        result=1;

        break;
      }
      else
      {
        *(QWORD *)(currentcpuinfo->eptCloakList[i])=CloakedPages[i].PhysicalAddressData; //todo: Let the user specify a bitmask so writes go through to the executable page as well
        currentcpuinfo->eptCloakList[i]->WA=1;
        currentcpuinfo->eptCloakList[i]->RA=1;
        currentcpuinfo->eptCloakList[i]->XA=1;

        vmx_enableSingleStepMode();
        vmx_addSingleSteppingReason(currentcpuinfo, 2,i);
        currentcpuinfo->eptCloak_LastOperationWasWrite=evi.W;


        result=1;

        break;

      }

      csLeave(&currentcpuinfo->EPTPML4CS);


    }

  }
  csLeave(&CloakedPagesCS);


  return result;
}

int ept_handleCloakEventAfterStep(pcpuinfo currentcpuinfo,  int ID)
{
  sendstringf("ept_handleCloakEventAfterStep\n");
  //back to execute only
  csEnter(&CloakedPagesCS);

  if (currentcpuinfo->eptCloak_LastOperationWasWrite)
  {
    //apply the write as well (TODO: let the user specify what memory should be skipped when writing)
    if ((*(QWORD *)(currentcpuinfo->eptCloakList[ID]) & MAXPHYADDRMASKPB) == CloakedPages[ID].PhysicalAddressExecutable)
    {
      //this was a write and execute at the same time
    }
    else
    {

    }
  }


  csEnter(&currentcpuinfo->EPTPML4CS);
  *(QWORD *)(currentcpuinfo->eptCloakList[ID])=CloakedPages[ID].PhysicalAddressExecutable; //back to the executable state
  currentcpuinfo->eptCloakList[ID]->WA=0;
  currentcpuinfo->eptCloakList[ID]->RA=0;
  if (has_EPT_ExecuteOnlySupport)
    currentcpuinfo->eptCloakList[ID]->XA=1;
  else
    currentcpuinfo->eptCloakList[ID]->XA=0;
  csLeave(&currentcpuinfo->EPTPML4CS);


  csLeave(&CloakedPagesCS);

  vmx_disableSingleStepMode();

  return 0;
}


int ept_cloak_activate(QWORD physicalAddress)
{
  int i;
  int ID=-1;

  sendstringf("ept_cloak_activate(%6)\n", physicalAddress);

  physicalAddress=physicalAddress & MAXPHYADDRMASKPB;
  csEnter(&CloakedPagesCS);

  //check if this physical address is already cloaked and find a free position while doing so
  for (i=0; i<CloakedPagesPos; i++)
  {
    if (CloakedPages[i].PhysicalAddressExecutable==physicalAddress)
    {
      csLeave(&CloakedPagesCS);
      return 1; //already cloaked
    }

    if ((ID==-1) && (CloakedPages[i].PhysicalAddressExecutable==0) && (CloakedPages[i].PhysicalAddressData==0))
      ID=i;
  }

  if (ID==-1) //new one
  {
    if (CloakedPagesPos==CloakedPagesSize) //need to reallocate the list
    {
      CloakedPagesSize=(CloakedPagesSize+2)*2;
      CloakedPages=realloc(CloakedPages, sizeof(CloakedPageInfo)*CloakedPagesSize);

      if (CloakedPages==NULL)
      {
        nosendchar[getAPICID()]=0;
        sendstringf("CloakedPages realloc failed\n");
        csLeave(&CloakedPagesCS);
        return 0xcedead00;
      }
    }

    ID=CloakedPagesPos;
    CloakedPagesPos++;
  }

  CloakedPages[ID].Executable=mapPhysicalMemoryGlobal(physicalAddress, 4096);
  CloakedPages[ID].Data=malloc(4096);


  if (CloakedPages[ID].Data==NULL)
  {
    if (CloakedPages[ID].Executable)
      unmapPhysicalMemoryGlobal(CloakedPages[ID].Executable, 4096);

    sendstringf("CloakedPages alloc data copy failed\n");
    csLeave(&CloakedPagesCS);
    return 0xcedead01;
  }
  copymem(CloakedPages[ID].Data, CloakedPages[ID].Executable, 4096);
  CloakedPages[ID].PhysicalAddressExecutable=physicalAddress;
  CloakedPages[ID].PhysicalAddressData=VirtualToPhysical(CloakedPages[ID].Data);

  //map in the physical address descriptor for all CPU's as execute only
  pcpuinfo currentcpuinfo=firstcpuinfo;
  while (currentcpuinfo)
  {
    csEnter(&currentcpuinfo->EPTPML4CS);

    //make sure that the eptCloakList is at least as big as CloakedPagesSize
    if (currentcpuinfo->eptCloakListLength<CloakedPagesSize)
      currentcpuinfo->eptCloakList=realloc(currentcpuinfo->eptCloakList, CloakedPagesSize);

    QWORD PA=EPTMapPhysicalMemory(currentcpuinfo, physicalAddress, 1);
    currentcpuinfo->eptCloakList[ID]=mapPhysicalMemoryGlobal(PA, sizeof(EPT_PTE));

    //make it nonreadable
    EPT_PTE temp=*(currentcpuinfo->eptCloakList[ID]);
    if (has_EPT_ExecuteOnlySupport)
      temp.XA=1;
    else
      temp.XA=0; //going to be slow

    temp.RA=0;
    temp.WA=0;


    *(currentcpuinfo->eptCloakList[ID])=temp;

    csLeave(&currentcpuinfo->EPTPML4CS);

    currentcpuinfo=currentcpuinfo->next;
  }




  csLeave(&CloakedPagesCS);
  return 0;
}

int ept_cloak_deactivate(QWORD physicalAddress)
{
  int i;
  int found=0;
  physicalAddress=physicalAddress & MAXPHYADDRMASKPB;
  csEnter(&CloakedPagesCS);

  for (i=0; i<CloakedPagesPos; i++)
  {
    if (CloakedPages[i].PhysicalAddressExecutable==physicalAddress)
    {
      //found it
      pcpuinfo currentcpuinfo=firstcpuinfo;
      while (currentcpuinfo)
      {
        //mark as full access
        EPT_PTE temp=*(currentcpuinfo->eptCloakList[i]);
        temp.RA=1;
        temp.WA=1;
        temp.XA=1;
        *(currentcpuinfo->eptCloakList[i])=temp;

        unmapPhysicalMemoryGlobal(currentcpuinfo->eptCloakList[i], sizeof(EPT_PTE));
        currentcpuinfo->eptCloakList[i]=NULL;

        currentcpuinfo=currentcpuinfo->next;
      }

      found=1;
      CloakedPages[i].PhysicalAddressExecutable=0;
      CloakedPages[i].PhysicalAddressData=0;
      free(CloakedPages[i].Data);
      CloakedPages[i].Data=NULL;
      unmapPhysicalMemoryGlobal(CloakedPages[i].Executable, 4096);
      CloakedPages[i].Executable=NULL;


    }
  }

  csLeave(&CloakedPagesCS);

  //if there where cloak event events pending, then next time they violate, the normal handler will make it RWX on the address it should
  return (found==1);
}

int ept_cloak_readOriginal(pcpuinfo currentcpuinfo,  VMRegisters *registers, QWORD physicalAddress, QWORD destination)
{
  int error;
  int i,ID=-1;
  physicalAddress=physicalAddress & MAXPHYADDRMASKPB;

  QWORD pagefault;

  void *dest=mapVMmemory(currentcpuinfo, destination, 4096,&error, &pagefault);

  if (error==2)
    return raisePagefault(currentcpuinfo, pagefault);

  csEnter(&CloakedPagesCS);
  for (i=0; i<CloakedPagesPos; i++)
    if (CloakedPages[i].PhysicalAddressExecutable==physicalAddress)
    {
      ID=i;
      break;
    }

  if (ID!=-1)
  {
    void *src=mapPhysicalMemory(CloakedPages[i].PhysicalAddressExecutable, 4096);
    copymem(dest,src,4096);
    registers->rax=0;

    unmapPhysicalMemory(src,4096);
  }
  else
    registers->rax=1;

  csLeave(&CloakedPagesCS);

  unmapVMmemory(dest,4096);


  vmwrite(vm_guest_rip,vmread(vm_guest_rip)+vmread(vm_exit_instructionlength));
  return 0;
}

int ept_cloak_writeOriginal(pcpuinfo currentcpuinfo,  VMRegisters *registers, QWORD physicalAddress, QWORD source)
{
  int error;
  int i,ID=-1;
  physicalAddress=physicalAddress & MAXPHYADDRMASKPB;

  QWORD pagefault;

  void *src=mapVMmemory(currentcpuinfo, source, 4096,&error, &pagefault);

  if (error==2)
    return raisePagefault(currentcpuinfo, pagefault);

  csEnter(&CloakedPagesCS);
  for (i=0; i<CloakedPagesPos; i++)
    if (CloakedPages[i].PhysicalAddressExecutable==physicalAddress)
    {
      ID=i;
      break;
    }

  if (ID!=-1)
  {
    void *dest=mapPhysicalMemory(CloakedPages[i].PhysicalAddressExecutable, 4096);
    copymem(dest,src,4096);
    registers->rax=0;

    unmapPhysicalMemory(dest,4096);
  }
  else
    registers->rax=1;

  csLeave(&CloakedPagesCS);

  unmapVMmemory(src,4096);


  vmwrite(vm_guest_rip,vmread(vm_guest_rip)+vmread(vm_exit_instructionlength));
  return 0;
}

int ept_cloak_changeregonbp(QWORD physicalAddress, PCHANGEREGONBPINFO changereginfo)
{
  int i;
  int result=1;
  QWORD physicalBase=physicalAddress & MAXPHYADDRMASKPB;
  ept_cloak_activate(physicalBase); //just making sure

  csEnter(&CloakedPagesCS);
  for (i=0; i<CloakedPagesPos; i++)
  {
    if (CloakedPages[i].PhysicalAddressExecutable==physicalBase)
    {
      //found it.  Create an int3 bp at that spot
      int ID=-1;
      int offset=physicalAddress & 0xfff;
      unsigned char *executable=CloakedPages[i].Executable;

      //
      csEnter(&ChangeRegBPListCS);
      int j;
      for (j=0; j<ChangeRegBPListPos; j++)
      {
        if (ChangeRegBPList[j].Active==0)
        {
          ID=j;
          break;
        }
      }
      if (ID==-1)
      {
        ID=ChangeRegBPListPos;
        ChangeRegBPListPos++;
        if (ChangeRegBPListPos>=ChangeRegBPListSize) //realloc the list
        {
          ChangeRegBPListSize=(ChangeRegBPListSize+2)*2;
          ChangeRegBPList=realloc(ChangeRegBPList, sizeof(ChangeRegBPEntry)*ChangeRegBPListSize);
        }
      }


      ChangeRegBPList[ID].PhysicalAddress=physicalAddress;
      ChangeRegBPList[ID].originalbyte=executable[offset];
      ChangeRegBPList[ID].changereginfo=*changereginfo;
      ChangeRegBPList[ID].CloakedRangeIndex=i;
      ChangeRegBPList[ID].Active=1;

      executable[offset]=0xcc; //int3 bp's will happen now (even on other CPU's)

      csLeave(&ChangeRegBPListCS);
      result=0;
    }
  }

  csLeave(&CloakedPagesCS);

  return result;
}

int ept_cloak_removechangeregonbp(QWORD physicalAddress)
{
  int i;
  int result=1;
  csEnter(&CloakedPagesCS);
  csEnter(&ChangeRegBPListCS);
  for (i=0; i<ChangeRegBPListPos; i++)
  {
    if ((ChangeRegBPList[i].Active) && (ChangeRegBPList[i].PhysicalAddress==physicalAddress))
    {
      unsigned char *executable=(unsigned char *)CloakedPages[ChangeRegBPList[i].CloakedRangeIndex].Executable;
      executable[physicalAddress & 0xfff]=ChangeRegBPList[i].originalbyte;
      ChangeRegBPList[i].Active=0;
      result=0;
    }
  }

  csLeave(&ChangeRegBPListCS);
  csLeave(&CloakedPagesCS);

  return result;
}

int ept_handleSoftwareBreakpoint(pcpuinfo currentcpuinfo)
{
  //check if it is a cloaked instruction
  int i;
  int result=1;


  //convert RIP into a physical address

  int notpaged;
  QWORD PA=getPhysicalAddressVM(currentcpuinfo, vmread(vm_guest_rip), &notpaged);

  if (notpaged==0) //should be since it's a software interrupt...
  {
    csEnter(&CloakedPagesCS);
    csEnter(&ChangeRegBPListCS);
    for (i=0; i<ChangeRegBPListPos; i++)
    {
      if (ChangeRegBPList[i].PhysicalAddress==PA)
      {
        if (ChangeRegBPList[i].Active)
        {
          //it's a match
          //Todo: Only change if the processID matches  (todo: Add a getProcessID option provided by the OS based caller)
          //For now, make sure that the physical page is not shared, or that the register change is compatible with different processes (e.g kernelmode only, or a Flag change)

          if ((ChangeRegBPList[i].changereginfo.Flags.changeRIP==0) || (ChangeRegBPList[i].changereginfo.newRIP==vmread(vm_guest_rip)))
          {
            //RIP does not change

            //restore the original byte
            int offset=ChangeRegBPList[i].PhysicalAddress & 0xfff;

            unsigned char *executable=(unsigned char *)CloakedPages[ChangeRegBPList[i].CloakedRangeIndex].Executable;
            executable[offset]=ChangeRegBPList[i].originalbyte;

            //setup single step mode
            vmx_enableSingleStepMode();
            vmx_addSingleSteppingReason(currentcpuinfo, 3,i); //change reg on bp, restore int3 bp

            vmwrite(vm_guest_rip, vmread(vm_guest_rip)-1);
            result=0;
            break;
          }
        }
        else
        {
          //probably a stale breakpoint event that was waiting for the spinlock (what are the changes that there was a 0xcc at the exact same spot a previous bp was set)
          //try again
          //todo: keep a try again counter
          vmwrite(vm_guest_rip, vmread(vm_guest_rip)-1);
          result=0;
        }
      }
    }

    csLeave(&ChangeRegBPListCS);
    csLeave(&CloakedPagesCS);
  }

  return result;
}


int ept_handleSoftwareBreakpointAfterStep(pcpuinfo currentcpuinfo UNUSED,  int ID)
{
  int result=1;
  csEnter(&CloakedPagesCS);
  csEnter(&ChangeRegBPListCS);
  if (ChangeRegBPList[ID].Active)
  {
    int i;
    QWORD PA=ChangeRegBPList[ID].PhysicalAddress;
    QWORD PABase=PA & MAXPHYADDRMASKPB;

    //find this PA in the cloaked pages list
    for (i=0; i<CloakedPagesPos; i++)
    {
      if (CloakedPages[i].PhysicalAddressExecutable==PABase)
      {
        int offset=PA-PABase;
        unsigned char *executable=(unsigned char*)CloakedPages[i].Executable;
        executable[offset]=0xcc; //set the breakpoint back
        result=0;
        break;
      }
    }


  }

  csLeave(&ChangeRegBPListCS);
  csLeave(&CloakedPagesCS);

  return result;
}


/*
 * WATCH
 */


int getFreeWatchID()
/*
 * scan through the watches for an unused spot, if not found, reallocate the list
 * pre: The watchlistCS has been locked
 */
{
  int i,j;
  sendstringf("getFreeWatchID\n");
  for (i=0; i<eptWatchListPos; i++)
  {
    if (eptWatchList[i].Active==0)
    {
      sendstringf("Found a non active entry at index %d\n", i);
      return i;
    }
  }

  //still here
  if (eptWatchListPos<eptWatchListSize)
  {
    sendstringf("eptWatchListPos(%d)<eptWatchListSize(%d)\n", eptWatchListPos, eptWatchListSize);
    return eptWatchListPos++;
  }

  sendstringf("Reallocating the list\n");

  //still here, realloc
  i=eptWatchListSize;
  eptWatchListSize=(eptWatchListSize+2)*2;
  eptWatchList=realloc(eptWatchList, eptWatchListSize*sizeof(EPTWatchEntry));

  for (j=i; j<eptWatchListSize; j++)
    eptWatchList[j].Active=0;

  eptWatchListPos++;

  return i;
}

void saveStack(pcpuinfo currentcpuinfo, unsigned char *stack) //stack is 4096 bytes
{
  int error;
  QWORD pagefaultaddress;
  int size=4096;

  zeromemory(stack, 4096);
  //copy it but don't care about pagefaults (if there is a pagefault I 'could' trigger a pf and then wait and try again, but fuck it, it's not 'that' important
  unsigned char *gueststack=(unsigned char *)mapVMmemoryEx(currentcpuinfo, vmread(vm_guest_rsp), 4096, &error, &pagefaultaddress, 1);

  if (error)
  {
    if (error==2)
      size=pagefaultaddress-vmread(vm_guest_rsp);
    else
      return;
  }

  copymem(stack, gueststack, size);
  unmapVMmemory(gueststack, size);
}

void fillPageEventBasic(PageEventBasic *peb, VMRegisters *registers)
{
  peb->VirtualAddress=vmread(vm_guest_linear_address);
  peb->PhysicalAddress=vmread(vm_guest_physical_address);
  peb->CR3=vmread(vm_guest_cr3);
  peb->FSBASE=vmread(vm_guest_fs_base);
  peb->GSBASE=vmread(vm_guest_gs_base);
  peb->RAX=registers->rax;
  peb->RBX=registers->rbx;
  peb->RCX=registers->rcx;
  peb->RDX=registers->rdx;
  peb->RSI=registers->rsi;
  peb->RDI=registers->rdi;
  peb->R8=registers->r8;
  peb->R9=registers->r9;
  peb->R10=registers->r10;
  peb->R11=registers->r11;
  peb->R12=registers->r12;
  peb->R13=registers->r13;
  peb->R14=registers->r14;
  peb->R15=registers->r15;
  peb->RBP=registers->rbp;
  peb->RSP=vmread(vm_guest_rsp);
  peb->RIP=vmread(vm_guest_rip);
  peb->CS=vmread(vm_guest_cs);
  peb->DS=vmread(vm_guest_ds);
  peb->ES=vmread(vm_guest_es);
  peb->SS=vmread(vm_guest_ss);
  peb->FS=vmread(vm_guest_fs);
  peb->GS=vmread(vm_guest_gs);
  peb->Count=0;
}


inline int ept_isWatchIDPerfectMatch(QWORD address, int ID)
{
  return ((eptWatchList[ID].Active) &&
          (
             (address>=eptWatchList[ID].PhysicalAddress) &&
             (address<eptWatchList[ID].PhysicalAddress+eptWatchList[ID].Size)
           )
          );
}

inline int ept_isWatchIDMatch(QWORD address, int ID)
/*
 * pre: address is already page aligned
 */
{
  return ((eptWatchList[ID].Active) && ((eptWatchList[ID].PhysicalAddress & 0xfffffffffffff000ULL) == address));
}

int ept_getWatchID(QWORD address)
/*
 * returns -1 if not in a page being watched
 * Note that there can be multiple active on the same page
 */
{
  int i;
  address=address & 0xfffffffffffff000ULL;
  for (i=0; i<eptWatchListPos; i++)
    if (ept_isWatchIDMatch(address, i))
      return i;

  return -1;
}

BOOL ept_handleWatchEvent(pcpuinfo currentcpuinfo, VMRegisters *registers, PFXSAVE64 fxsave, QWORD PhysicalAddress)
{
  int ID;
  int logentrysize;
  int i;

  if (eptWatchListPos==0)
    return FALSE;

  csEnter(&eptWatchListCS);
  ID=ept_getWatchID(PhysicalAddress);

  if (ID==-1)
  {
    csLeave(&eptWatchListCS);
    return FALSE;
  }


  QWORD RIP=vmread(vm_guest_rip);
  QWORD RSP=vmread(vm_guest_rsp);
  QWORD PhysicalAddressBase=PhysicalAddress & 0xfffffffffffff000ULL;
  EPT_VIOLATION_INFO evi;
  evi.ExitQualification=vmread(vm_exit_qualification);

  //nosendchar[getAPICID()]=0;
  sendstringf("Handling something that resembles watch ID %d\n", ID);


  //figure out which access it is really (in case of multiple on the same page)

  for (i=ID; i<eptWatchListPos; i++)
  {
    if (ept_isWatchIDMatch(PhysicalAddressBase, i))
    {
      if (eptWatchList[ID].Type==0)
      {
        //must be a write operation error
        if ((evi.W) && (evi.WasWritable==0)) //write operation and writable was 0
        {
          ID=i;

          if (ept_isWatchIDPerfectMatch(PhysicalAddress, i))
            break;
        }
      }
      else
      {
        //must be a read or write operation
        if (((evi.W) && (evi.WasWritable==0)) || ((evi.R) && (evi.WasReadable==0)))  //write operation and writable was 0 or read and readable was 0
        {
          ID=i;
          if (ept_isWatchIDPerfectMatch(PhysicalAddress, i))
            break;
        }
      }
    }
  }

  sendstringf("Handling watch ID %d\n", ID);

  //todo: release the eptWatchListCS and obtain only the log

  //ID is now set to the most logical watch(usually there is no conflicts, and even if there is, no biggie. But still)

  if ((currentcpuinfo->eptWatchList[ID]->XA) && (currentcpuinfo->eptWatchList[ID]->RA) && (currentcpuinfo->eptWatchList[ID]->WA))
  {
    sendstringf("This entry was already marked with full access (check caches)\n");
  }

  //run once
  currentcpuinfo->eptWatchList[ID]->XA=1;
  currentcpuinfo->eptWatchList[ID]->RA=1;
  currentcpuinfo->eptWatchList[ID]->WA=1;

  sendstringf("Page is accessible. Doing single step\n");

  vmx_enableSingleStepMode();
  vmx_addSingleSteppingReason(currentcpuinfo, 1, ID);


  //save this state?
  if ((evi.R==0) && (evi.X==1))
  {
    sendstringf("This was an execute operation and no read. No need to log\n", ID);
    csLeave(&eptWatchListCS);
    return TRUE; //execute operation (this cpu doesn't support execute only)
  }

  if (eptWatchList[ID].CopyInProgress) //a copy operation is in progress
  {
    sendstringf("This watchlist is currently being copied, not logging this\n");
    csLeave(&eptWatchListCS);
    return TRUE;
  }

  if (((eptWatchList[ID].Options & EPTO_LOG_ALL)==0) &&
     (
      (PhysicalAddress<eptWatchList[ID].PhysicalAddress) ||
      (PhysicalAddress>=eptWatchList[ID].PhysicalAddress+eptWatchList[ID].Size)
      ))
  {
    sendstringf("Not logging all and the physical address is not in the exact range\n");
    csLeave(&eptWatchListCS);
    return TRUE; //no need to log it
  }


  //scan if this RIP is already in the list

  switch (eptWatchList[ID].Log->entryType)
  {
    case 0: logentrysize=sizeof(PageEventBasic); break;
    case 1: logentrysize=sizeof(PageEventExtended); break;
    case 2: logentrysize=sizeof(PageEventBasicWithStack); break;
    case 3: logentrysize=sizeof(PageEventExtendedWithStack); break;
  }

  sendstringf("Want to log this. Type=%d EntrySize=%d\n", eptWatchList[ID].Log->entryType, logentrysize);

  for (i=0; (DWORD)i<eptWatchList[ID].Log->numberOfEntries; i++)
  {
    PageEventBasic *peb=(PageEventBasic *)((QWORD)(&eptWatchList[ID].Log->pe.basic[0])+i*logentrysize);
    //every type starts with a PageEventBasic

    if (peb->RIP==RIP)
    {
      sendstringf("This RIP is already logged");
      //it's already in the list
      if ((eptWatchList[ID].Options & EPTO_MULTIPLERIP)==0)
      {
        sendstringf(" and EPTO_MULTIPLERIP is 0.  Not logging (just increase count)\n");
        peb->Count++;
        csLeave(&eptWatchListCS);
        return TRUE; //no extra RIP's
      }
      else
        sendstringf(" but EPTO_MULTIPLERIP is 1, so checking register states\n");

      //still here, so multiple RIP's are ok. check if it matches the other registers
      if (
          (peb->RSP==RSP) &&
          (peb->RBP==registers->rbp) &&
          (peb->RAX==registers->rax) &&
          (peb->RBX==registers->rbx) &&
          (peb->RCX==registers->rcx) &&
          (peb->RDX==registers->rdx) &&
          (peb->RSI==registers->rsi) &&
          (peb->RDI==registers->rdi) &&
          (peb->R8==registers->r8) &&
          (peb->R9==registers->r9) &&
          (peb->R10==registers->r10) &&
          (peb->R11==registers->r11) &&
          (peb->R12==registers->r12) &&
          (peb->R13==registers->r13) &&
          (peb->R14==registers->r14) &&
          (peb->R15==registers->r15)
        )
      {
        sendstringf("  The registers match the state so skipping the log. (Just increase count)\n");
        peb->Count++;
        csLeave(&eptWatchListCS);
        return TRUE; //already in the list
      }

    }

  }

  //still here, so not in the list
  sendstringf("Checks out ok. Not yet in the list\n");

  if (eptWatchList[ID].Log->numberOfEntries>=eptWatchList[ID].Log->maxNumberOfEntries)
  {
    sendstringf("List is full");
    if ((eptWatchList[ID].Options & EPTO_GROW_WHENFULL)==0)
    {
      sendstringf(". Discarding event\n");
      eptWatchList[ID].Log->missedEntries++;
      csLeave(&eptWatchListCS);
      return TRUE; //can't add more
    }

    //reallocate the buffer
    int newmax=eptWatchList[ID].Log->numberOfEntries*2;
    PPageEventListDescriptor temp=realloc(eptWatchList[ID].Log, sizeof(PageEventListDescriptor)+logentrysize*newmax);
    if (temp!=NULL)
    {
      sendstringf(" so growing it\n");
      eptWatchList[ID].Log=temp;
      eptWatchList[ID].Log->numberOfEntries=newmax;
    }
    else
    {
      sendstringf(" and out of memory\n");

      eptWatchList[ID].Options=eptWatchList[ID].Options & (~EPTO_GROW_WHENFULL); //stop trying
      eptWatchList[ID].Log->missedEntries++;
      csLeave(&eptWatchListCS);
      return TRUE; //can't add more
    }

  }

  //still here, so not in the list, and still room
  //add it



  i=eptWatchList[ID].Log->numberOfEntries;
  switch (eptWatchList[ID].Log->entryType)
  {
    case PE_BASIC:
    {
      fillPageEventBasic(&eptWatchList[ID].Log->pe.basic[i], registers);
      break;
    }

    case PE_EXTENDED:
    {
      fillPageEventBasic(&eptWatchList[ID].Log->pe.extended[i].basic, registers);
      eptWatchList[ID].Log->pe.extended[i].fpudata=*fxsave;
      break;
    }

    case PE_BASICSTACK:
    {
      fillPageEventBasic(&eptWatchList[ID].Log->pe.basics[i].basic, registers);
      saveStack(currentcpuinfo, eptWatchList[ID].Log->pe.basics[i].stack);
      break;
    }

    case PE_EXTENDEDSTACK:
    {
      fillPageEventBasic(&eptWatchList[ID].Log->pe.extendeds[i].basic, registers);
      eptWatchList[ID].Log->pe.extended[i].fpudata=*fxsave;
      saveStack(currentcpuinfo, eptWatchList[ID].Log->pe.extendeds[i].stack);
      break;
    }
  }

  eptWatchList[ID].Log->numberOfEntries++;


  sendstringf("Added it to the list. numberOfEntries for ID %d is now %d\n", ID, eptWatchList[ID].Log->numberOfEntries);


  return TRUE;

}

int ept_handleWatchEventAfterStep(pcpuinfo currentcpuinfo,  int ID)
{
  sendstringf("ept_handleWatchEventAfterStep %d\n", ID);
  vmx_disableSingleStepMode();

  if (eptWatchList[ID].Type==0)
  {
    sendstringf("Write type. So making it unwritable\n");
    currentcpuinfo->eptWatchList[ID]->WA=0;
  }
  else
  {
    sendstringf("read type. So making it unreadable\n");
    currentcpuinfo->eptWatchList[ID]->RA=0;
    currentcpuinfo->eptWatchList[ID]->WA=0;
    if (has_EPT_ExecuteOnlySupport)
      currentcpuinfo->eptWatchList[ID]->XA=1;
    else
      currentcpuinfo->eptWatchList[ID]->XA=0;
  }


  return 0;
}

VMSTATUS ept_watch_retrievelog(int ID, QWORD results, DWORD *resultSize, DWORD *offset, QWORD *errorcode)
{
  *errorcode=0;
  sendstringf("ept_watch_retrievelog(ID=%d)\n", ID);



  if (ID>=eptWatchListPos) //out of range
  {
    sendstringf("Invalid ID\n");
    vmwrite(vm_guest_rip,vmread(vm_guest_rip)+vmread(vm_exit_instructionlength));
    *errorcode=1; //invalid ID
    return VM_OK;
  }

  csEnter(&eptWatchListCS);

  if (eptWatchList[ID].Active==0) //not active
  {
    sendstringf("Inactive ID\n");
    vmwrite(vm_guest_rip,vmread(vm_guest_rip)+vmread(vm_exit_instructionlength));
    *errorcode=3; //inactive ID

    csLeave(&eptWatchListCS);
    return VM_OK;
  }


  int entrysize=0;

  switch (eptWatchList[ID].Log->entryType)
  {
    case 0: entrysize=sizeof(PageEventBasic); break;
    case 1: entrysize=sizeof(PageEventExtended); break;
    case 2: entrysize=sizeof(PageEventBasicWithStack); break;
    case 3: entrysize=sizeof(PageEventExtendedWithStack); break;
  }

  sendstringf("entrytype=%d (size = %d)\n", eptWatchList[ID].Log->entryType, entrysize);

  DWORD sizeneeded=sizeof(PageEventListDescriptor)+eptWatchList[ID].Log->numberOfEntries*entrysize;
  sendstringf("sizeneeded=%d\n", sizeneeded);
  sendstringf("resultsize=%d\n", *resultSize);

  if (*resultSize < sizeneeded)
  {
    sendstringf("Too small\n");
    *resultSize=sizeneeded;
    vmwrite(vm_guest_rip,vmread(vm_guest_rip)+vmread(vm_exit_instructionlength));
    *errorcode=2; //invalid size
    csLeave(&eptWatchListCS);
    return VM_OK;
  }

  if (results==0)
  {
    sendstringf("results==0\n");
    vmwrite(vm_guest_rip,vmread(vm_guest_rip)+vmread(vm_exit_instructionlength));
    *errorcode=4; //results==0
    csLeave(&eptWatchListCS);
    return VM_OK;
  }

  int sizeleft=sizeneeded-*offset; //decrease bytes left by bytes already copied

  sendstringf("params->copied=%d", sizeneeded-*offset);

  eptWatchList[ID].CopyInProgress=1;

  int error;
  QWORD pagefaultaddress;
  QWORD destinationaddress=results+*offset;
  int blocksize=sizeleft;
  if (blocksize>16*4096)
    blocksize=16*4096;

  unsigned char *source=(unsigned char *)eptWatchList[ID].Log;
  unsigned char *destination=mapVMmemoryEx(NULL, destinationaddress, blocksize, &error, &pagefaultaddress,1);

  if (error)
  {
    if (error==2)
    {
      blocksize=pagefaultaddress-destinationaddress;
    }
    else
    {
      vmwrite(vm_guest_rip,vmread(vm_guest_rip)+vmread(vm_exit_instructionlength));
      *errorcode=0x1000+error; //map error
      csLeave(&eptWatchListCS);
      return 0;
    }
  }

  if (blocksize)
  {
    copymem(destination, source, blocksize);
    unmapVMmemory(destination, blocksize);

    *offset=*offset+blocksize;
  }

  if (error==2)
  {
    csLeave(&eptWatchListCS);
    return raisePagefault(getcpuinfo(), pagefaultaddress);
  }


  if (*offset>=sizeneeded) //> would be weird....
  {
    //once all data has been copied
    eptWatchList[ID].Log->numberOfEntries=0;
    eptWatchList[ID].CopyInProgress=0;
    vmwrite(vm_guest_rip,vmread(vm_guest_rip)+vmread(vm_exit_instructionlength));
  }

  return VM_OK;
}


int ept_watch_activate(QWORD PhysicalAddress, int Size, int Type, DWORD Options, int MaxEntryCount, int *outID)
{
  int result=0;
  sendstringf("ept_watch_activate(%6, %d, %d, %x, %d, %6)\n", PhysicalAddress, Size, Options, MaxEntryCount, outID);

  if (MaxEntryCount)
    return 1;

  csEnter(&eptWatchListCS);

  int ID=getFreeWatchID();
  int structtype=(Options >> 2) & 3;
  int structsize;

  sendstringf("getFreeWatchID() returned %d .  eptWatchListPos=%d\n", ID, eptWatchListPos);
  switch (structtype)
  {
    case 0: structsize=sizeof(PageEventBasic); break;
    case 1: structsize=sizeof(PageEventExtended); break;
    case 2: structsize=sizeof(PageEventBasicWithStack); break;
    case 3: structsize=sizeof(PageEventExtendedWithStack); break;
  }

  //make sure it doesn't pass a page boundary
  //todo: recursively spawn more watches if needed

  if (((PhysicalAddress+Size) & 0xfffffffffffff000ULL) > (PhysicalAddress & 0xfffffffffffff000ULL))
       eptWatchList[ID].Size=0x1000-(PhysicalAddress & 0xfff);

  eptWatchList[ID].PhysicalAddress=PhysicalAddress;
  eptWatchList[ID].Size=Size;
  eptWatchList[ID].Type=Type;
  eptWatchList[ID].Log=malloc(sizeof(PageEventListDescriptor)+structsize*MaxEntryCount);
  zeromemory(eptWatchList[ID].Log, sizeof(PageEventListDescriptor)+structsize*MaxEntryCount);

  eptWatchList[ID].Log->ID=ID;
  eptWatchList[ID].Log->entryType=structtype;
  eptWatchList[ID].Log->numberOfEntries=0;
  eptWatchList[ID].Log->maxNumberOfEntries=MaxEntryCount;

  eptWatchList[ID].Options=Options;
  sendstringf("Configured ept watch. Activating ID %d\n", ID);

  eptWatchList[ID].Active=1;

  //for each CPU mark this page as non writable/readable
  pcpuinfo c=firstcpuinfo;
  while (c)
  {
    csEnter(&c->EPTPML4CS);

    QWORD PA_EPTE=EPTMapPhysicalMemory(c, PhysicalAddress, 1);

    if (c->eptWatchListLength<eptWatchListSize) //realloc
      c->eptWatchList=realloc(c->eptWatchList, eptWatchListSize*sizeof(EPT_PTE));

    c->eptWatchList[ID]=mapPhysicalMemoryGlobal(PA_EPTE, sizeof(EPT_PTE)); //can use global as it's not a quick map/unmap procedure

    EPT_PTE temp=*c->eptWatchList[ID]; //using temp in case the cpu doesn't support a XA of 1 with an RA of 0

    if (Type==0)
      temp.WA=0;
    else
    if (Type==1)
    {
      if (has_EPT_ExecuteOnlySupport)
        temp.XA=1;
      else
        temp.XA=0;
      temp.WA=0;
      temp.RA=0;
    }
    *(c->eptWatchList[ID])=temp;

    csLeave(&c->EPTPML4CS);

    c=c->next;
  }

  //test if needed:  SetPageToWriteThrough(currentcpuinfo->eptwatchlist[ID].EPTEntry);
  //check out 28.3.3.4  (might not be needed due to vmexit, but do check anyhow)
  //EPTINV



  //everything ok, return success:

  sendstringf("Passing ID to the called\n");
  *outID=ID;


  csLeave(&eptWatchListCS);

  return result;
}

int ept_watch_deactivate(int ID)
/*
 * disable a watch
 * It's possible that a watch event is triggered between now and the end of this function.
 * That will result in a ept_violation event, which causes it to not see that there is a watch event for this
 * As a result, that DBVM threat will try to map the physical address , and if it's already mapped, set it to full access
 * In short: it's gonna be ok
 */
{
  int i;
  int hasAnotherOne=0;
  sendstringf("ept_disableWatch(%d)", ID);

  csEnter(&eptWatchListCS);

  QWORD PhysicalBase=eptWatchList[ID].PhysicalAddress & 0xfffffffffffff000ULL;

  for (i=0; i<eptWatchListPos; i++)
  {
    if ((i!=ID) && ept_isWatchIDMatch(PhysicalBase, i))
    {
      //matches
      if (eptWatchList[i].Type==eptWatchList[ID].Type) //don't undo
        hasAnotherOne=1;
    }
  }

  if (hasAnotherOne==0)
  {
    pcpuinfo c=firstcpuinfo;

    while (c)
    {
      //undo

      csEnter(&c->EPTPML4CS);

      EPT_PTE temp=*(c->eptWatchList[ID]);
      if (eptWatchList[ID].Type==0)
      {
        sendstringf("This was a write entry. Making it writable\n");
        temp.WA=1;
      }
      else
      {
        sendstringf("This was an access entry. Making it readable and writable");
        temp.RA=1;
        temp.WA=1;
        if (has_EPT_ExecuteOnlySupport==0)
        {
          sendstringf("And executable as this cpu does not support execute only pages\n");
          temp.XA=1;
        }
      }

      *(c->eptWatchList[ID])=temp;

      csLeave(&c->EPTPML4CS);

      unmapPhysicalMemoryGlobal(c->eptWatchList[ID], sizeof(EPT_PTE));
      c->eptWatchList[ID]=NULL;
      c=c->next;
    }
  }
  else
  {
    sendstringf("hasAnotherOne is set\n");
  }

  eptWatchList[ID].Active=0;
  csLeave(&eptWatchListCS);
  return 0;
}


inline int MTC_RPS(int mt1, int mt2)
{
  //memory type cache rock paper scissors
  if (mt1==mt2)
    return mt1;

  if ((mt1==MTC_UC) || (mt2==MTC_UC))
      return MTC_UC;

  if (((mt1==MTC_WT) && (mt2==MTC_WB)) || ((mt2==MTC_WT) && (mt1==MTC_WB)) )
    return MTC_WT;

  if ((mt1==MTC_WP) || (mt2==MTC_WP))
    return MTC_WP;

  if ((mt1==MTC_WB) || (mt2!=MTC_WB))
    return mt2;
  else
    return min(mt1,mt2);
}

typedef struct _MEMRANGE
{
  QWORD startaddress;
  QWORD size;
  int memtype;
} MEMRANGE, *PMEMRANGE;

criticalSection memoryrangesCS;
MEMRANGE *memoryranges;
int memoryrangesLength;
int memoryrangesPos;

void getMTRRMapInfo(QWORD startaddress, QWORD size, int *fullmap, int *memtype)
/*
 * pre: startaddress is aligned on the boundary you wish to map
 *
 * post: fullmap is 1 if the size can be fully mapped without conflicts (border crossings)
 */
{
  //note: the list is sorted
  QWORD starta=startaddress;
  QWORD stopa=startaddress+size-1;
  int morethan1=0;
  int i;

  *memtype=MTRRDefType.TYPE;
  *fullmap=1;

  csEnter(&memoryrangesCS);
  for (i=0; i<memoryrangesPos; i++)
  {
    QWORD startb,stopb;
    startb=memoryranges[i].startaddress;
    stopb=memoryranges[i].startaddress+memoryranges[i].size-1;

    if ((starta <= stopb) && (startb <= stopa))
    {
      //overlap, check the details
      if ((starta>=startb) && (stopa<=stopb)) //falls completely within the region, so can be fully mapped.
      {
        if (morethan1==0)
          *memtype=memoryranges[i].memtype;//set the memory type
        else
          *memtype=MTC_RPS(memoryranges[i].memtype, *memtype); //set the memory type based on the two combined types

        morethan1++;
      }
      else
      {
        *fullmap=0; //mark as not fully mappable, go one level lower(this also happens on overlaps where a second part doesn't fit, shouldn't happen often)

        break;
      }


    }

    if (stopa<startb) //reached a startaddress higher than my stopaddress, which means every other item will be as well
      break;
  }
  csLeave(&memoryrangesCS);
}

void addToMemoryRanges(QWORD address, QWORD size, int type)
/*
 * pre: memoryrangesCS lock has been aquired
 */
{
  int i;
  int insertpos=-1;


  if (size==0) return;

  //add memory for a new entry
  if (memoryrangesPos==memoryrangesLength)
  {
    realloc2(memoryranges, memoryrangesLength*sizeof(MEMRANGE), 2*memoryrangesLength*sizeof(MEMRANGE));
    memoryrangesLength=memoryrangesLength*2;
  }

  for (i=0; i<memoryrangesPos; i++)
  {
    if (memoryranges[i].startaddress>address)
    {
      //insert here
      insertpos=i;
      break;
    }
  }

  if (insertpos==-1)
    insertpos=memoryrangesPos;

  for (i=memoryrangesPos; i>insertpos; i--)
    memoryranges[i]=memoryranges[i-1];

  memoryranges[insertpos].startaddress=address;
  memoryranges[insertpos].size=size;
  memoryranges[insertpos].memtype=type;
  memoryrangesPos++;
}

void initMemTypeRanges()
//builds an array of memory ranges and their cache
{
  int i;
  csEnter(&memoryrangesCS);

  memoryrangesPos=0;

  if (memoryranges==NULL)
  {
    memoryrangesLength=32;
    memoryranges=malloc2(sizeof(MEMRANGE)*memoryrangesLength);
  }

  QWORD startaddress=0;
  QWORD size=0;
  int memtype=MTRRDefType.TYPE;

  if ((MTRRCapabilities.FIX && MTRRDefType.FE))
  {

    QWORD FIX64K_00000=readMSR(IA32_MTRR_FIX64K_00000);
    QWORD FIX16K_80000=readMSR(IA32_MTRR_FIX16K_80000);
    QWORD FIX16K_A0000=readMSR(IA32_MTRR_FIX16K_A0000);
    QWORD FIX4K_C0000 =readMSR(IA32_MTRR_FIX4K_C0000);
    QWORD FIX4K_C8000 =readMSR(IA32_MTRR_FIX4K_C8000);
    QWORD FIX4K_D0000 =readMSR(IA32_MTRR_FIX4K_D0000);
    QWORD FIX4K_D8000 =readMSR(IA32_MTRR_FIX4K_D8000);
    QWORD FIX4K_E0000 =readMSR(IA32_MTRR_FIX4K_E0000);
    QWORD FIX4K_E8000 =readMSR(IA32_MTRR_FIX4K_E8000);
    QWORD FIX4K_F0000 =readMSR(IA32_MTRR_FIX4K_F0000);
    QWORD FIX4K_F8000 =readMSR(IA32_MTRR_FIX4K_F8000);
    //check the fixed range mtrs

    while (startaddress+size<0x100000)
    {
      int type;
      int sizeinc=0;

      switch (startaddress+size)
      {
        case 0 ... 0x7ffff:
        {
          QWORD types=FIX64K_00000;
          int index=(startaddress+size) >> 16;
          type=(types >> (index*8)) & 0xf;
          sizeinc=64*1024;
          break;
        }

        case 0x80000 ... 0x9ffff:
        {
          QWORD types=FIX16K_80000;
          int index=((startaddress+size)-0x80000) >> 14;
          type=(types >> (index*8)) & 0xf;
          sizeinc=16*1024;
          break;
        }

        case 0xa0000 ... 0xbffff:
        {
          QWORD types=FIX16K_A0000;
          int index=((startaddress+size)-0xa0000) >> 14;
          type=(types >> (index*8)) & 0xf;
          sizeinc=16*1024;
          break;
        }

        case 0xc0000 ... 0xc7fff:
        {
          QWORD types=FIX4K_C0000;
          int index=((startaddress+size)-0xc0000) >> 12;
          type=(types >> (index*8)) & 0xf;
          sizeinc=4*1024;
          break;
        }

        case 0xc8000 ... 0xcffff:
        {
          QWORD types=FIX4K_C8000;
          int index=((startaddress+size)-0xc8000) >> 12;
          type=(types >> (index*8)) & 0xf;
          sizeinc=4*1024;
          break;
        }

        case 0xd0000 ... 0xd7fff:
        {
          QWORD types=FIX4K_D0000;
          int index=((startaddress+size)-0xd0000) >> 12;
          type=(types >> (index*8)) & 0xf;
          sizeinc=4*1024;
          break;
        }

        case 0xd8000 ... 0xdffff:
        {
          QWORD types=FIX4K_D8000;
          int index=((startaddress+size)-0xd8000) >> 12;
          type=(types >> (index*8)) & 0xf;
          sizeinc=4*1024;
          break;
        }

        case 0xe0000 ... 0xe7fff:
        {
          QWORD types=FIX4K_E0000;
          int index=((startaddress+size)-0xe0000) >> 12;
          type=(types >> (index*8)) & 0xf;
          sizeinc=4*1024;
          break;
        }

        case 0xe8000 ... 0xeffff:
        {
          QWORD types=FIX4K_E8000;
          int index=((startaddress+size)-0xe8000) >> 12;
          type=(types >> (index*8)) & 0xf;
          sizeinc=4*1024;
          break;
        }

        case 0xf0000 ... 0xf7fff:
        {
          QWORD types=FIX4K_F0000;
          int index=((startaddress+size)-0xf0000) >> 12;
          type=(types >> (index*8)) & 0xf;
          sizeinc=4*1024;
          break;
        }

        case 0xf8000 ... 0xfffff:
        {
          QWORD types=FIX4K_F8000;
          int index=((startaddress+size)-0xf8000) >> 12;
          type=(types >> (index*8)) & 0xf;
          sizeinc=4*1024;
          break;
        }


      }

      if (type==memtype) //same type, continue
        size+=sizeinc;
      else //type changed
      {
        if (memtype!=MTRRDefType.TYPE) //old type wasn't the default, add it to the list
          addToMemoryRanges(startaddress, size, memtype);


        //start a new region
        startaddress=startaddress+size;
        size=0;
        memtype=type;
      }
    }

    if ((size) && (memtype!=MTRRDefType.TYPE)) //last region needs to be added as well
      addToMemoryRanges(startaddress, size, memtype);
  }

  //check the var fields
  for (i=0; i<MTRRCapabilities.VCNT; i++)
  {
    QWORD base=readMSR(IA32_MTRR_PHYSBASE0+i*2);
    QWORD mask=readMSR(IA32_MTRR_PHYSMASK0+i*2);
    int memtype=base & 0xff;

    if ((mask & (1<<11)) && (memtype!=MTRRDefType.TYPE)) //valid
    {
      // Address_Within_Range AND PhysMask = PhysBase AND PhysMask

      //strip of useless bits
      base=base & MAXPHYADDRMASKPB;
      mask=mask & MAXPHYADDRMASKPB;

      //find the highest 0 bit in the mask to find the region (this allows for the shitty “discontinuous” ranges)
      int j;

      for (j=MAXPHYADDR-1; j>12; j--)
      {
        if ((mask & ((QWORD)1<<j))==0)
        {
          QWORD size=((QWORD)1<<(j+1)); //the last bit with 1
          addToMemoryRanges(base, size, memtype);
          break;
        }
      }
    }
  }

  if (loadedOS==0)
  {
    addToMemoryRanges(VirtualToPhysical((void *)0x00400000), 0x00400000, MTC_WP);
  }

  csLeave(&memoryrangesCS);
}



int remapMTRRTypes(QWORD address UNUSED, QWORD size UNUSED, int type UNUSED)
{
  //called by the MSR write handler when MTRR registers get changed
  return 1;
}

int handleMSRWrite_MTRR(void)
//called when an MTRR msr is written. Figures out what regions have been modified
{
  return 1;
}

QWORD EPTMapPhysicalMemory(pcpuinfo currentcpuinfo, QWORD physicalAddress, int forcesmallpage)
/*
 * Maps the physical address into the EPT map.
 * Returns the physical address of the EPT entry describing this page
 */
{
  int pml4index;
  int pagedirptrindex;
  int pagedirindex;
  int pagetableindex;
  QWORD PA;
  VirtualAddressToIndexes(physicalAddress, &pml4index, &pagedirptrindex, &pagedirindex, &pagetableindex);


  PEPT_PML4E pml4=NULL;
  PEPT_PDPTE pagedirptr=NULL;
  PEPT_PDE pagedir=NULL;
  PEPT_PTE pagetable=NULL;

  csEnter(&currentcpuinfo->EPTPML4CS);

  pml4=mapPhysicalMemory(currentcpuinfo->EPTPML4, 4096);
  if (pml4[pml4index].RA==0)
  {
    sendstringf("allocating pagedirptr\n");
    //allocate a pagedirptr table
    void *temp=malloc2(4096);
    zeromemory(temp,4096);
    *(QWORD*)(&pml4[pml4index])=VirtualToPhysical(temp) & MAXPHYADDRMASKPB;
    pml4[pml4index].RA=1;
    pml4[pml4index].WA=1;
    pml4[pml4index].XA=1;
  }

  PA=(*(QWORD*)(&pml4[pml4index])) & MAXPHYADDRMASKPB;
  pagedirptr=mapPhysicalMemory(PA, 4096);

  PA+=8*pagedirptrindex;

  if (pml4)
  {
    unmapPhysicalMemory(pml4, 4096);
    pml4=NULL;
  }

  if (forcesmallpage && pagedirptr[pagedirptrindex].RA && (pagedirptr[pagedirptrindex].BIG)) //it's a big page, so the physical address points to the actual memory. Clear everything
    *(QWORD *)&pagedirptr[pagedirptrindex]=0;

  if (pagedirptr[pagedirptrindex].RA==0)
  {
    //check if there is a MTRR in this 1GB range that doesn't set the MTRRDefType.TYPE, if not, map as 1GB using deftype
    if (has_EPT_1GBsupport && (forcesmallpage==0))
    {
      QWORD GuestAddress1GBAlign=(physicalAddress & 0xFFFFFFFFC0000000ULL) & MAXPHYADDRMASKPB;
      int fullmap, memtype;
      getMTRRMapInfo(GuestAddress1GBAlign,0x40000000, &fullmap, &memtype);

      if (fullmap)
      {
        //map as a 1GB page
        sendstringf("mapping %6 as a 1GB page with memtype %d\n", GuestAddress1GBAlign, memtype);
        *(QWORD*)(&pagedirptr[pagedirptrindex])=GuestAddress1GBAlign;
        pagedirptr[pagedirptrindex].RA=1;
        pagedirptr[pagedirptrindex].WA=1;
        pagedirptr[pagedirptrindex].XA=1;
        pagedirptr[pagedirptrindex].BIG=1;
        pagedirptr[pagedirptrindex].MEMTYPE=memtype;
        unmapPhysicalMemory(pagedirptr, 4096);

        csLeave(&currentcpuinfo->EPTPML4CS);
        return PA;
      }
    }

    //still here, try a pagedir
    void *temp=malloc2(4096);
    zeromemory(temp,4096);
    *(QWORD*)(&pagedirptr[pagedirptrindex])=VirtualToPhysical(temp) & MAXPHYADDRMASKPB;
    pagedirptr[pagedirptrindex].RA=1;
    pagedirptr[pagedirptrindex].WA=1;
    pagedirptr[pagedirptrindex].XA=1;
  }

  PA=(*(QWORD*)(&pagedirptr[pagedirptrindex])) & MAXPHYADDRMASKPB;
  pagedir=mapPhysicalMemory(PA, 4096);

  PA+=8*pagedirindex;

  if (pagedirptr)
  {
    unmapPhysicalMemory(pagedirptr, 4096);
    pagedirptr=NULL;
  }

  if (forcesmallpage && pagedir[pagedirindex].RA && (pagedir[pagedirindex].BIG)) //it's a big page, so the physical address points to the actual memory. Clear everything
    *(QWORD *)&pagedir[pagedirindex]=0;


  if (pagedir[pagedirindex].RA==0)
  {
    if (has_EPT_2MBSupport && (forcesmallpage==0))
    {
      QWORD GuestAddress2MBAlign=(physicalAddress & 0xFFFFFFFFFFE00000ULL) & MAXPHYADDRMASKPB;
      int memtype, fullmap;
      getMTRRMapInfo(GuestAddress2MBAlign,0x200000, &fullmap, &memtype);

      if (fullmap)
      {
        sendstringf("mapping %6 as a 2MB page with memtype %d\n", GuestAddress2MBAlign, memtype);
        *(QWORD*)(&pagedir[pagedirindex])=GuestAddress2MBAlign & MAXPHYADDRMASKPB;
        pagedir[pagedirindex].RA=1;
        pagedir[pagedirindex].WA=1;
        pagedir[pagedirindex].XA=1;
        pagedir[pagedirindex].BIG=1;
        pagedir[pagedirindex].MEMTYPE=memtype;
        unmapPhysicalMemory(pagedir, 4096);

        csLeave(&currentcpuinfo->EPTPML4CS);
        return PA;
      }
    }

    //still here, try a pagetable
    void *temp=malloc2(4096);
    zeromemory(temp,4096);
    *(QWORD*)(&pagedir[pagedirindex])=VirtualToPhysical(temp) & MAXPHYADDRMASKPB;
    pagedir[pagedirindex].RA=1;
    pagedir[pagedirindex].WA=1;
    pagedir[pagedirindex].XA=1;
  }

  //still here, so not mapped as a pagedir entry
  PA=(*(QWORD*)(&pagedir[pagedirindex])) & MAXPHYADDRMASKPB;
  pagetable=mapPhysicalMemory(PA, 4096);

  PA+=8*pagetableindex;

  if (pagedir)
  {
    unmapPhysicalMemory(pagedir,4096);
    pagedir=NULL;
  }

  if (pagetable[pagetableindex].RA==0)
  {
    int memtype, fullmap;
    getMTRRMapInfo(physicalAddress & MAXPHYADDRMASKPB,0x1000, &fullmap, &memtype);
    if (!fullmap)
    {
      sendstring("Assertion Fail: fullmap is false for a 1 page range");
      while (1);
    }

    sendstringf("mapping %6 as a 4KB page with memtype %d\n", physicalAddress & MAXPHYADDRMASKPB, memtype);
    *(QWORD*)(&pagetable[pagetableindex])=physicalAddress & MAXPHYADDRMASKPB;
    pagetable[pagetableindex].RA=1;
    pagetable[pagetableindex].WA=1;
    pagetable[pagetableindex].XA=1;
    pagetable[pagetableindex].MEMTYPE=memtype;
  }
  else
  {
    //else already mapped
    sendstringf("This physical address (%6) was already mapped\n", physicalAddress);

    //change it to full access
    pagetable[pagetableindex].RA=1;
    pagetable[pagetableindex].WA=1;
    pagetable[pagetableindex].XA=1;
  }

  unmapPhysicalMemory(pagetable,4096);

  csLeave(&currentcpuinfo->EPTPML4CS);

  return PA;
}

VMSTATUS handleEPTViolation(pcpuinfo currentcpuinfo, VMRegisters *vmregisters UNUSED, PFXSAVE64 fxsave UNUSED)
{
  //EPT_VIOLATION_INFO vi;
  sendstring("handleEPTViolation\n");


  VMExit_idt_vector_information idtvectorinfo;
  idtvectorinfo.idtvector_info=vmread(vm_idtvector_information);

  if (idtvectorinfo.valid)
  {
    //handle this EPT event and reinject the interrupt
    VMEntry_interruption_information newintinfo;
    newintinfo.interruption_information=0;

    newintinfo.interruptvector=idtvectorinfo.interruptvector;
    newintinfo.type=idtvectorinfo.type;
    newintinfo.haserrorcode=idtvectorinfo.haserrorcode;
    newintinfo.valid=idtvectorinfo.valid; //should be 1...
    vmwrite(vm_entry_exceptionerrorcode, vmread(vm_idtvector_error)); //entry errorcode
    vmwrite(0x4016, newintinfo.interruption_information); //entry info field
    vmwrite(0x401a, vmread(vm_exit_instructionlength)); //entry instruction length
  }

 //vi.ExitQualification=vmread(vm_exit_qualification);

  QWORD GuestAddress=vmread(vm_guest_physical_address);

  if (ept_handleWatchEvent(currentcpuinfo, vmregisters, fxsave, GuestAddress))
    return 0;



  //check for cloak
  if (ept_handleCloakEvent(currentcpuinfo, GuestAddress))
    return 0;

  //still here, so not a watch or cloak
  sendstringf("Mapping %6\n", GuestAddress);
  EPTMapPhysicalMemory(currentcpuinfo, GuestAddress, 0);


  return 0;

}

VMSTATUS handleEPTMisconfig(pcpuinfo currentcpuinfo UNUSED, VMRegisters *vmregisters UNUSED)
{
  sendstring("handleEPTMisconfig\n");
  //could have been a timing misconfig, try again

  VMExit_idt_vector_information idtvectorinfo;
  idtvectorinfo.idtvector_info=vmread(vm_idtvector_information);
  if (idtvectorinfo.valid)
  {
    //handle this EPT event and reinject the interrupt
    VMEntry_interruption_information newintinfo;
    newintinfo.interruption_information=0;

    newintinfo.interruptvector=idtvectorinfo.interruptvector;
    newintinfo.type=idtvectorinfo.type;
    newintinfo.haserrorcode=idtvectorinfo.haserrorcode;
    newintinfo.valid=idtvectorinfo.valid; //should be 1...
    vmwrite(vm_entry_exceptionerrorcode, vmread(vm_idtvector_error)); //entry errorcode
    vmwrite(0x4016, newintinfo.interruption_information); //entry info field
    vmwrite(0x401a, vmread(vm_exit_instructionlength)); //entry instruction length
  }

  return 0;
}
