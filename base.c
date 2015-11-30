/************************************************************************

This code forms the base of the operating system you will
build.  It has only the barest rudiments of what you will
eventually construct; yet it contains the interfaces that
allow test.c and z502.c to be successfully built together.

Revision History:
1.0 August 1990
1.1 December 1990: Portability attempted.
1.3 July     1992: More Portability enhancements.
Add call to sample_code.
1.4 December 1992: Limit (temporarily) printout in
interrupt handler.  More portability.
2.0 January  2000: A number of small changes.
2.1 May      2001: Bug fixes and clear STAT_VECTOR
2.2 July     2002: Make code appropriate for undergrads.
Default program start is in test0.
3.0 August   2004: Modified to support memory mapped IO
3.1 August   2004: hardware interrupt runs on separate thread
3.11 August  2004: Support for OS level locking
4.0  July    2013: Major portions rewritten to support multiple threads
************************************************************************/

#include             "global.h"
#include             "syscalls.h"
#include             "protos.h"
#include             "string.h"
#include             "student.h"
//#include			 "state_printer.c"

static				 QNode		 *currentNode;
static				 PCB		 *currentPCB;
static			     long		 counter=1;
static      int     msgCount = 0;
LQueue				 *timer_queue;
QNode				 *timer_node;
LQueue				 *ready_queue;
QNode				 *ready_node;
LQueue				 *suspend_queue;
QNode				 *suspend_node;
LQueue				 *message_queue;
MNode				 *message_node;
INT32				 LockResult,TimeLockResult,LockResultPrinter;
static		int		 counterofprocess = 0;
int enablePrinter = 0;
int enableDiskPrint = 0;
char action[SP_LENGTH_OF_ACTION];

void   interrupt_handler(void);
void   fault_handler(void);
void   start_timer(INT32 SLEEPTIME);
void   dispatcher();
void   en_ready_queue_by_priority(QNode *enReadyNode);
PCB    create_process(SYSTEM_CALL_DATA *SystemCallData);
long   get_process_id_by_name(char *name);
int    terminate_process(SYSTEM_CALL_DATA *SystemCallData);
void   terminate_itself();
int    suspend_process(SYSTEM_CALL_DATA *SystemCallData);
int    resume_process(SYSTEM_CALL_DATA *SystemCallData);
void   schedule_printer();

// These loacations are global and define information about the page table
extern UINT16        *Z502_PAGE_TBL_ADDR;
extern INT16         Z502_PAGE_TBL_LENGTH;

extern void          *TO_VECTOR[];

char                 *call_names[] = { "mem_read ", "mem_write",
"read_mod ", "get_time ", "sleep    ",
"get_pid  ", "create   ", "term_proc",
"suspend  ", "resume   ", "ch_prior ",
"send     ", "receive  ", "disk_read",
"disk_wrt ", "def_sh_ar" };


/************************************************************************
INTERRUPT_HANDLER
When the Z502 gets a hardware interrupt, it transfers control to
this routine in the OS.
************************************************************************/
void interrupt_handler(void) {
	INT32              device_id;
	INT32              status;
	INT32              Index = 0;
	INT32			   currentTime,sleepTime;
	QNode			   *outNode, *timerExistNode, *lastTimerExistNode;

	timerExistNode = malloc(sizeof(QNode));
	timerExistNode = timer_queue->front;
	lastTimerExistNode = malloc(sizeof(QNode));
	outNode = malloc(sizeof(QNode));

	// Get cause of interrupt
	MEM_READ(Z502InterruptDevice, &device_id);
	// Set this device as target of our query
	MEM_WRITE(Z502InterruptDevice, &device_id);
	// Now read the status of this device
	MEM_READ(Z502InterruptStatus, &status);
	
	// Your code will go there. It should include a switch statement for 
	// each device - you have only the timer right now.
	// Check that the timer reported a success code.
	// Call a routine you write that does the work described later.
	switch (device_id){
		case TIMER_INTERRUPT:
			READ_MODIFY(MEMORY_INTERLOCK_BASE + 10, DO_LOCK, SUSPEND_UNTIL_LOCKED, &TimeLockResult);
			MEM_READ(Z502ClockStatus,&currentTime);
			while (timerExistNode->next != NULL){
				lastTimerExistNode = timerExistNode;
				timerExistNode = timerExistNode->next;
				if ((timerExistNode->p_c_b).wakeuptime <= currentTime){
					outNode->p_c_b = timerExistNode->p_c_b;
					outNode->next = NULL;
					//remove from timer queue
					lastTimerExistNode->next = timerExistNode->next;
					//add to ready_queue
					en_ready_queue_by_priority(outNode);
				}
				else{
					break;
				}
			}
			if (timerExistNode->next != NULL)
			{
				CALL(MEM_READ(Z502ClockStatus, &currentTime));
				sleepTime = (timerExistNode->p_c_b).wakeuptime - currentTime;
				CALL(MEM_WRITE(Z502TimerStart, &sleepTime));
			}
			READ_MODIFY(MEMORY_INTERLOCK_BASE + 10, DO_UNLOCK, SUSPEND_UNTIL_LOCKED, &TimeLockResult);
			break;
		default:
			READ_MODIFY(MEMORY_INTERLOCK_BASE + 15, DO_LOCK, SUSPEND_UNTIL_LOCKED, &TimeLockResult);
			printf("Hold on!....\n");
			READ_MODIFY(MEMORY_INTERLOCK_BASE + 15, DO_UNLOCK, SUSPEND_UNTIL_LOCKED, &TimeLockResult);
			break;
	}
	
	MEM_WRITE(Z502InterruptClear, &Index);
}                                       /* End of interrupt_handler */

/************************************************************************
FAULT_HANDLER
The beginning of the OS502.  Used to receive hardware faults.
************************************************************************/

void fault_handler(void)
{
	INT32       device_id;
	INT32       status;
	INT32       Index = 0;
	// Get cause of interrupt
	MEM_READ(Z502InterruptDevice, &device_id);
	// Set this device as target of our query
	MEM_WRITE(Z502InterruptDevice, &device_id);
	// Now read the status of this device
	MEM_READ(Z502InterruptStatus, &status);

	printf("Fault_handler: Found vector type %d with value %d\n",
		device_id, status);
	// Clear out this device - we're done with it
	if (device_id == 4 && status == 0)
	{
		printf("Clock | Timer has error!\n");
		Z502Halt();
	}
	MEM_WRITE(Z502InterruptClear, &Index);
}                                       /* End of fault_handler */

void start_timer(INT32 SLEEPTIME){
	INT32               Temp, Status, currentTime;
	INT32				WakeUpTime;
	QNode				*tempNode, *timerExistNode, *lastTimerExistNode;
	
	//find current time to calculate wakeuptime
	CALL(MEM_READ(Z502ClockStatus, &currentTime));
	WakeUpTime = currentTime + SLEEPTIME;
	currentPCB->wakeuptime = WakeUpTime;
	
	//add current pcb to a Node
	tempNode = malloc(sizeof(QNode));
	tempNode->p_c_b = *currentPCB;

	//denote node in timer_queue
	timerExistNode = malloc(sizeof(QNode));
	timerExistNode = timer_queue->front;
	lastTimerExistNode = malloc(sizeof(QNode));
	
	//timer_queue is empty,just add into timer queue
	if (timerExistNode->next == NULL){
		tempNode->next = NULL;
		timerExistNode->next = tempNode;
		timer_queue->rear = tempNode;
		
	}
	//choose where to put tempNode based on wakeupTime of PCB
	else{
		while (timerExistNode->next != NULL){
			lastTimerExistNode = timerExistNode;
			timerExistNode = timerExistNode->next;
			if ((tempNode->p_c_b).wakeuptime <= (timerExistNode->p_c_b).wakeuptime){
				//add to timer queue before timer exist node
				lastTimerExistNode->next = tempNode;
				tempNode->next = timerExistNode;
				//printf("%d\n", currentPCB->wakeuptime);
				break;
			}
		}
		//printf("%d\n", currentPCB->wakeuptime);
		//if at the end tempNode is still bigger than timerExistNode, add to the end
		if ((tempNode->p_c_b).wakeuptime > (timerExistNode->p_c_b).wakeuptime){
			tempNode->next = NULL;
			timerExistNode->next = tempNode;
			timer_queue->rear = tempNode;
			
		}
	}
	//MEM_READ(Z502TimerStatus, &Status);
	/*if (Status == DEVICE_FREE)
		printf("Got expected result for Status of Timer\n");
	else
		printf("Got erroneous result for Status of Timer\n");*/
	Temp = SLEEPTIME; /* You pick the time units */
	MEM_WRITE(Z502TimerStart, &Temp);
	/*MEM_READ(Z502TimerStatus, &Status);
	if (Status == DEVICE_IN_USE)
		printf("Got expected result for Status of Timer\n");
	else
		printf("Got erroneous result for Status of Timer\n");*/
	// after the current node is inserted into timerQueue, just do dispatcher() to get a new node for current node
	dispatcher();
}

void dispatcher(){
	while(ready_queue->front->next == NULL)
	{
		// if no process in the whole program, just halt, as it meaningless to wait to forever
		if ((timer_queue->front->next == NULL)&&(suspend_queue->front->next == NULL))
		{
			
			Z502Halt();
		}
		
		if ((timer_queue->front->next == NULL) && (suspend_queue->front->next != NULL))
		{

			*currentPCB = suspend_queue->front->next->p_c_b;
			suspend_queue->front->next = suspend_queue->front->next->next;
			Z502SwitchContext(SWITCH_CONTEXT_SAVE_MODE, &(currentPCB->context));
			return;
		}
		//currentPCB = NULL;
		CALL(Z502Idle());
		strncpy(action, "IDEL", 8);
		schedule_printer();
	}
	READ_MODIFY(MEMORY_INTERLOCK_BASE, DO_LOCK, SUSPEND_UNTIL_LOCKED, &LockResult);
	*currentPCB = ready_queue->front->next->p_c_b;
	
	//pop up the first node from readyQueue
	ready_queue->front->next = ready_queue->front->next->next;
	READ_MODIFY(MEMORY_INTERLOCK_BASE, DO_UNLOCK, SUSPEND_UNTIL_LOCKED, &LockResult);
	schedule_printer();
	// switch to current node process
	Z502SwitchContext(SWITCH_CONTEXT_SAVE_MODE, &(currentPCB->context));
}

void en_ready_queue_by_priority(QNode *enReadyNode){
	QNode				*readyExistNode, *lastReadyExistNode;
	readyExistNode = malloc(sizeof(QNode));
	readyExistNode = ready_queue->front;
	lastReadyExistNode = malloc(sizeof(QNode));
	if (readyExistNode->next == NULL)
	{
		readyExistNode->next = enReadyNode;
	}
	else
	{
		while (readyExistNode->next != NULL){
			lastReadyExistNode = readyExistNode;
			readyExistNode = readyExistNode->next;
			if ((enReadyNode->p_c_b).priority < (readyExistNode->p_c_b).priority){
				//smaller priority  is more favorable
				enReadyNode->next = readyExistNode;
				lastReadyExistNode->next = enReadyNode;
				break;
			}
		}
		if ((enReadyNode->p_c_b).priority >= (readyExistNode->p_c_b).priority){
			enReadyNode->next = NULL;
			readyExistNode->next = enReadyNode;
		}
	}
}

PCB create_process(SYSTEM_CALL_DATA *SystemCallData)
{
	void *new_context;
	PCB  *createPCB;
	// generate a PCB node with the information we know
	createPCB = (PCB*)malloc(sizeof(PCB));
	strcpy(createPCB->name, (char*)SystemCallData->Argument[0]);
	Z502MakeContext(&new_context, (void *)SystemCallData->Argument[1], USER_MODE);
	createPCB->context = new_context;
	createPCB->priority = (long)SystemCallData->Argument[2];
	createPCB->pid = counter++;

	//generate a tempNode to contain the PCB we get
	QNode *tempNode;
	tempNode = (QNode*)malloc(sizeof(QNode));
	tempNode->p_c_b = *createPCB;
	tempNode->next = NULL;

	//generate a readyExistNode to contain node in ready_queue
	QNode *readyExistNode;
	readyExistNode = (QNode*)malloc(sizeof(QNode));
	readyExistNode = ready_queue->front;
	if (readyExistNode->next == NULL && ((tempNode->p_c_b).priority != ILLEGAL_PRIORITY)){
		(tempNode->p_c_b).error = 0;                  //0 means create a correct process
		counterofprocess++;
		if (counterofprocess > 15){
			(tempNode->p_c_b).error = -3;                // -3 means out of line
		}
		else{
			en_ready_queue_by_priority(tempNode);
		}
		return tempNode->p_c_b;
	}
	else if(readyExistNode->next == NULL && ((tempNode->p_c_b).priority == ILLEGAL_PRIORITY)){
		(tempNode->p_c_b).error = -1;                //-1 means illegal priority
		return tempNode->p_c_b;
	}
	else{
		while (readyExistNode->next != NULL){
			readyExistNode = readyExistNode->next;
			if (strcmp((readyExistNode->p_c_b).name, (tempNode->p_c_b).name) == 0)        //same name   
			{// same name
				(tempNode->p_c_b).error = -2;        //-2 means same name
				return tempNode->p_c_b;
			}
		}
		if ((tempNode->p_c_b).priority != ILLEGAL_PRIORITY){
			(tempNode->p_c_b).error = 0;             //0 means create a correct process
			counterofprocess++;
			if (counterofprocess > 15){
				(tempNode->p_c_b).error = -3;                // -3 means out of line
			}
			else{
				en_ready_queue_by_priority(tempNode);
			}
			return tempNode->p_c_b;
		}
		else{
			(tempNode->p_c_b).error = -1;            //-1 means illegal priority
			return tempNode->p_c_b;
		}
	}
	//...................................................
	
}

long get_process_id_by_name(char *name){
	//generate a existNode to contain node in ready_queue
	QNode *readyExistNode,*lastReadyExistNode;
	readyExistNode = malloc(sizeof(QNode));
	lastReadyExistNode = malloc(sizeof(QNode));
	readyExistNode = ready_queue->front;

	//printf("~~~~%s\n", (readyExistNode->next->p_c_b).name);
	QNode *timerExistNode;
	timerExistNode = malloc(sizeof(QNode));
	timerExistNode = timer_queue->front;
	
	QNode *suspendExistNode;
	suspendExistNode = malloc(sizeof(QNode));
	suspendExistNode = suspend_queue->front;

	if (strcmp(name, "") == 0)
	{
		return currentPCB->pid;
	}
	//printf("%s\n", (readyExistNode->p_c_b).name);
	while (readyExistNode->next != NULL){
		
		readyExistNode = readyExistNode->next;
		if (strcmp((readyExistNode->p_c_b).name, name) == 0){
			return (readyExistNode->p_c_b).pid;
		}
	}
	//printf("%d\n", (timerExistNode->next->p_c_b).pid);
	while (timerExistNode->next != NULL){
		timerExistNode = timerExistNode->next;
		if (strcmp((timerExistNode->p_c_b).name, name) == 0){
			return (timerExistNode->p_c_b).pid;
		}
	}
	while (suspendExistNode->next != NULL){
		suspendExistNode = suspendExistNode->next;
		if (strcmp((suspendExistNode->p_c_b).name, name) == 0){
			return (suspendExistNode->p_c_b).pid;
		}
	}
	return -3;                                       //-3 means no such name
}

int terminate_process(SYSTEM_CALL_DATA *SystemCallData){
	QNode				*timerExistNode, *lastTimerExistNode;
	QNode				*readyExistNode, *lastReadyExistNode;
	QNode				*suspendExistNode, *lastSuspendExistNode;

	timerExistNode = malloc(sizeof(QNode));
	timerExistNode = timer_queue->front;
	lastTimerExistNode = malloc(sizeof(QNode));
	

	readyExistNode = malloc(sizeof(QNode));
	readyExistNode = ready_queue->front;
	lastReadyExistNode = malloc(sizeof(QNode));
	

	suspendExistNode = malloc(sizeof(QNode));
	suspendExistNode = suspend_queue->front;
	lastSuspendExistNode = malloc(sizeof(QNode));
	
	//terminate by pid
	if ((long)SystemCallData->Argument[0] == currentPCB->pid)
	{
		terminate_itself();
		return 0;                    //0 means terminate successfully
	}
	/*else if ((long)SystemCallData->Argument[0] == -2){
		terminate_itself();
		while (timerExistNode->next != NULL){
			lastTimerExistNode = timerExistNode;
			timerExistNode = timerExistNode->next;
			if ((timerExistNode->p_c_b).pid == (long)SystemCallData->Argument[0]){
				//delete it
				lastTimerExistNode->next = timerExistNode->next;
				// free the node, which has been teminated from & removed from the Queue
				free(timerExistNode);
				
			}
		}
		while (readyExistNode->next != NULL){
			lastReadyExistNode = readyExistNode;
			readyExistNode = readyExistNode->next;
			if ((readyExistNode->p_c_b).pid == (long)SystemCallData->Argument[0]){
				//delete it
				lastReadyExistNode->next = readyExistNode->next;
				// free the node, which has been teminated from & removed from the Queue
				free(readyExistNode);
				
			}
		}
		while (suspendExistNode->next != NULL){
			lastSuspendExistNode = suspendExistNode;
			suspendExistNode = suspendExistNode->next;
			if ((suspendExistNode->p_c_b).pid == (long)SystemCallData->Argument[0]){
				//delete it
				lastSuspendExistNode->next = suspendExistNode->next;
				// free the node, which has been teminated from & removed from the Queue
				free(suspendExistNode);
				
			}
		}
		return 0;
	}*/

	while (timerExistNode->next != NULL){
		lastTimerExistNode = timerExistNode;
		timerExistNode = timerExistNode->next;
		if ((timerExistNode->p_c_b).pid == (long)SystemCallData->Argument[0]){
			//delete it
			lastTimerExistNode->next = timerExistNode->next;
			// free the node, which has been teminated from & removed from the Queue
			free(timerExistNode);
			return 0;                //0 means terminate successfully
		}
	}
	while (readyExistNode->next != NULL){
		lastReadyExistNode = readyExistNode;
		readyExistNode = readyExistNode->next;
		if ((readyExistNode->p_c_b).pid == (long)SystemCallData->Argument[0]){
			//delete it
			lastReadyExistNode->next = readyExistNode->next;
			// free the node, which has been teminated from & removed from the Queue
			free(readyExistNode);
			return 0;                //0 means terminate successfully
		}
	}
	while (suspendExistNode->next != NULL){
		lastSuspendExistNode = suspendExistNode;
		suspendExistNode = suspendExistNode->next;
		if ((suspendExistNode->p_c_b).pid == (long)SystemCallData->Argument[0]){
			//delete it
			lastSuspendExistNode->next = suspendExistNode->next;
			// free the node, which has been teminated from & removed from the Queue
			free(suspendExistNode);
			return 0;                //0 means terminate successfully
		}
	}
	return -1;                       //-1 means terminate incorrectly
}

void terminate_itself(){
	/*QNode				*timerExistNode, *lastTimerExistNode;
	QNode				*readyExistNode, *lastReadyExistNode;
	QNode				*suspendExistNode, *lastSuspendExistNode;

	timerExistNode = malloc(sizeof(QNode));
	timerExistNode = timer_queue->front;
	lastTimerExistNode = malloc(sizeof(QNode));


	readyExistNode = malloc(sizeof(QNode));
	readyExistNode = ready_queue->front;
	lastReadyExistNode = malloc(sizeof(QNode));


	suspendExistNode = malloc(sizeof(QNode));
	suspendExistNode = suspend_queue->front;
	lastSuspendExistNode = malloc(sizeof(QNode));

	while (timerExistNode->next != NULL){
		lastTimerExistNode = timerExistNode;
		timerExistNode = timerExistNode->next;
		if ((timerExistNode->p_c_b).pid == currentPCB->pid){
			//delete it
			lastTimerExistNode->next = timerExistNode->next;
			// free the node, which has been teminated from & removed from the Queue
			free(timerExistNode);
		}
	}
	while (readyExistNode->next != NULL){
		lastReadyExistNode = readyExistNode;
		readyExistNode = readyExistNode->next;
		if ((readyExistNode->p_c_b).pid == currentPCB->pid){
			//delete it
			lastReadyExistNode->next = readyExistNode->next;
			// free the node, which has been teminated from & removed from the Queue
			free(readyExistNode);
		}
	}
	while (suspendExistNode->next != NULL){
		lastSuspendExistNode = suspendExistNode;
		suspendExistNode = suspendExistNode->next;
		if ((suspendExistNode->p_c_b).pid == currentPCB->pid){
			//delete it
			lastSuspendExistNode->next = suspendExistNode->next;
			// free the node, which has been teminated from & removed from the Queue
			free(suspendExistNode);
		}
	}*/
	//currentPCB = NULL;
	dispatcher();
}

int suspend_process(SYSTEM_CALL_DATA *SystemCallData){
	QNode				*timerExistNode, *lastTimerExistNode;
	QNode				*readyExistNode, *lastReadyExistNode;
	QNode				*suspendExistNode, *lastSuspendExistNode;

	timerExistNode = malloc(sizeof(QNode));
	timerExistNode = timer_queue->front;
	lastTimerExistNode = malloc(sizeof(QNode));

	readyExistNode = malloc(sizeof(QNode));
	readyExistNode = ready_queue->front;
	lastReadyExistNode = malloc(sizeof(QNode));

	suspendExistNode = malloc(sizeof(QNode));
	suspendExistNode = suspend_queue->front;
	lastSuspendExistNode = malloc(sizeof(QNode));

	if ((long)SystemCallData->Argument[0] > 12)
	{
		return -2;                   //-2 means out of limit
	}

	while (timerExistNode->next != NULL){
		lastTimerExistNode = timerExistNode;
		timerExistNode = timerExistNode->next;
		if ((timerExistNode->p_c_b).pid == (long)SystemCallData->Argument[0]){
			//suspend it
			//remove from timer_queue
			lastTimerExistNode->next = timerExistNode->next;
			//add into suspend_queue
			timerExistNode->next = NULL;
			suspend_queue->rear->next = timerExistNode;
			suspend_queue->rear = timerExistNode;
			return 0;               //0 means successfully insert into suspend queue
		}
	}
	while (readyExistNode->next != NULL){
		lastReadyExistNode = readyExistNode;
		readyExistNode = readyExistNode->next;
		if ((readyExistNode->p_c_b).pid == (long)SystemCallData->Argument[0]){
			//suspend it
			//remove from ready_queue
			lastReadyExistNode->next = readyExistNode->next;
			//add into suspend_queue
			readyExistNode->next = NULL;
			suspend_queue->rear->next = readyExistNode;
			suspend_queue->rear = readyExistNode;
			return 0;               //0 means successfully insert into suspend queue
		}
	}
	while (suspendExistNode->next != NULL){
		lastSuspendExistNode = suspendExistNode;
		suspendExistNode = suspendExistNode->next;
		if ((suspendExistNode->p_c_b).pid == (long)SystemCallData->Argument[0]){
			return -1;               //-1 means already in suspend queue
		}
	}
	if ((long)SystemCallData->Argument[0] == -1)
	{
		return -2;                   //-2 means try to suspend itself
	}
}

int resume_process(SYSTEM_CALL_DATA *SystemCallData)
{
	QNode				*suspendExistNode, *lastSuspendExistNode,*outNode;
	suspendExistNode = malloc(sizeof(QNode));
	suspendExistNode = suspend_queue->front;
	lastSuspendExistNode = malloc(sizeof(QNode));
	outNode = malloc(sizeof(QNode));
	if ((long)SystemCallData->Argument[0] > 12)
	{
		return -2;                   //-2 means out of limit
	}
	while (suspendExistNode->next != NULL){
		lastSuspendExistNode = suspendExistNode;
		suspendExistNode = suspendExistNode->next;
		if ((suspendExistNode->p_c_b).pid == (long)SystemCallData->Argument[0]){
			outNode->p_c_b = suspendExistNode->p_c_b;
			outNode->next = NULL;
			//remove from suspend queue
			lastSuspendExistNode->next = suspendExistNode->next;
			//add to ready_queue
			en_ready_queue_by_priority(outNode);
			return 0;                   //0 means successfully resume
		}
	}
	return -1;                          //-1 means that pid doesn't exist
}

int change_priority_of_process(SYSTEM_CALL_DATA *SystemCallData){
	QNode				*timerExistNode, *lastTimerExistNode;
	QNode				*outNode, *readyExistNode, *lastReadyExistNode;
	QNode				*suspendExistNode, *lastSuspendExistNode;

	timerExistNode = malloc(sizeof(QNode));
	timerExistNode = timer_queue->front;
	lastTimerExistNode = malloc(sizeof(QNode));

	readyExistNode = malloc(sizeof(QNode));
	readyExistNode = ready_queue->front;
	lastReadyExistNode = malloc(sizeof(QNode));

	suspendExistNode = malloc(sizeof(QNode));
	suspendExistNode = suspend_queue->front;
	lastSuspendExistNode = malloc(sizeof(QNode));

	outNode = malloc(sizeof(QNode));

	while (timerExistNode->next != NULL){
		lastTimerExistNode = timerExistNode;
		timerExistNode = timerExistNode->next;
		if ((timerExistNode->p_c_b).pid == (long)SystemCallData->Argument[0]){
			(timerExistNode->p_c_b).priority = (long)SystemCallData->Argument[1];
			return 0;               //0 means successfully changed
		}
	}
	// need to be sorted!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
	while (readyExistNode->next != NULL){
		lastReadyExistNode = readyExistNode;
		readyExistNode = readyExistNode->next;
		if ((readyExistNode->p_c_b).pid == (long)SystemCallData->Argument[0]){
			(readyExistNode->p_c_b).priority = (long)SystemCallData->Argument[1];
			outNode->p_c_b = readyExistNode->p_c_b;
			outNode->next = NULL;
			en_ready_queue_by_priority(outNode);
			return 0;               //0 means successfully changed
		}
	}
	while (suspendExistNode->next != NULL){
		lastSuspendExistNode = suspendExistNode;
		suspendExistNode = suspendExistNode->next;
		if ((suspendExistNode->p_c_b).pid == (long)SystemCallData->Argument[0]){
			(suspendExistNode->p_c_b).priority = (long)SystemCallData->Argument[1];
			return 0;               //0 means successfully changed
		}
	}
	if ((long)SystemCallData->Argument[0] == -1 || (long)SystemCallData->Argument[0] == currentPCB->pid)
	{
		currentPCB->priority = (long)SystemCallData->Argument[1];
		return 0;                   //0 means successfully changed
	}
	
}

int send_message(SYSTEM_CALL_DATA *SystemCallData){
	MSG					*message;
	MNode				*msgNode;
	MNode				*messageExistNode, *lastMessageExistNode;
	QNode				*suspendExistNode, *lastSuspendExistNode;

	messageExistNode = malloc(sizeof(MNode));
	messageExistNode = message_queue->front;
	lastMessageExistNode = malloc(sizeof(MNode));

	suspendExistNode = malloc(sizeof(QNode));
	suspendExistNode = suspend_queue->front;
	lastSuspendExistNode = malloc(sizeof(QNode));
	
	if ((long)SystemCallData->Argument[0] > 12)
	{
		return -1;                   //-2 means out of limit
	}
	if ((long)SystemCallData->Argument[2] > 100)
	{
		return -1;                   //-1 means illegal length
	}
	// add the msg node at the end of msgQueue
	while (messageExistNode->next != NULL)
	{
		messageExistNode = messageExistNode->next;
	}
	message = (MSG*)malloc(sizeof(MSG));
	message->sid = currentPCB->pid;
	message->tid = (long)SystemCallData->Argument[0];
	message->length = (long)SystemCallData->Argument[2];
	strncpy(message->msg, (long)SystemCallData->Argument[1], (long)SystemCallData->Argument[2]);

	msgNode = malloc(sizeof(MNode));
	msgNode->m_s_g = *message;
	msgNode->next = NULL;

	messageExistNode->next = msgNode;
	
	// resume target pid node, if it is suspended
	/*if (suspendExistNode->next != NULL){

		resume_process((long)SystemCallData->Argument[0]);
	}
	/*if (currentPCB->pid == (long)SystemCallData->Argument[0]){
		suspend_process(currentPCB->pid);
		dispatcher();
	}*/
	return 0;                //0 means successfully send
}

int receive_message_new(long sid, char *msg, int msgLength, long *actualLength, long *actualSid){
	
		MNode				*messageExistNode, *lastMessageExistNode;
		QNode				*timerExistNode, *lastTimerExistNode;
		QNode				*suspendExistNode, *lastSuspendExistNode;
		QNode				*outNode, *tempNode;

		messageExistNode = malloc(sizeof(MNode));
		messageExistNode = message_queue->front;
		lastMessageExistNode = malloc(sizeof(MNode));

		timerExistNode = malloc(sizeof(QNode));
		timerExistNode = timer_queue->front;
		

		if (sid > 12)
		{
			return -1;                   //-2 means out of limit
		}
		while (messageExistNode->next != NULL){
			lastMessageExistNode = messageExistNode;
			messageExistNode = messageExistNode->next;
			if ((sid == -1 || (messageExistNode->m_s_g).sid == sid) && 
				(((messageExistNode->m_s_g).tid == -1 && (messageExistNode->m_s_g).sid != sid) || (messageExistNode->m_s_g).tid == currentPCB->pid)){
				// check buffer size, whether it is large enought to buffer the message
				if ((messageExistNode->m_s_g).length > msgLength)
				{
					return -1;                        // -1 means buffer size is small
				}
				else
				{
					strncpy(msg, (messageExistNode->m_s_g).msg, (messageExistNode->m_s_g).length);
					*actualLength = (messageExistNode->m_s_g).length;
					*actualSid = (messageExistNode->m_s_g).sid;
					//remove the node from msgQueue
					lastMessageExistNode->next = messageExistNode->next;
					return 0;                         // 0 means succesfully receive
				}
			}
		}

		if (timerExistNode->next != NULL)
		{
			while (timerExistNode->next != NULL)
			{
				CALL(Z502Idle());
			}
			outNode = malloc(sizeof(QNode));
			outNode->p_c_b = *currentPCB;
			outNode->next = NULL;
			en_ready_queue_by_priority(outNode);
			//or directly add to the end, to be designed
		}
		else{
			suspendExistNode = malloc(sizeof(QNode));
			suspendExistNode = suspend_queue->front;
			while (suspendExistNode->next != NULL){
				suspendExistNode = suspendExistNode->next;
			}
			tempNode = malloc(sizeof(QNode));
			tempNode->p_c_b = *currentPCB;
			tempNode->next = NULL;
			suspendExistNode->next = tempNode;
			//*currentPCB = ready_queue->front->next->p_c_b;

			//pop up the first node from readyQueue
			//ready_queue->front->next = ready_queue->front->next->next;
		}
		dispatcher();
		//
		receive_message_new(sid, msg, msgLength, actualLength, actualSid);
}

void schedule_printer()
{
	
	QNode				*timerExistNode, *lastTimerExistNode;
	QNode				*suspendExistNode, *lastSuspendExistNode;
	QNode				*readyExistNode, *lastReadyExistNode;
	timerExistNode = malloc(sizeof(QNode));
	timerExistNode = timer_queue->front;
	suspendExistNode = malloc(sizeof(QNode));
	suspendExistNode = suspend_queue->front;
	readyExistNode = malloc(sizeof(QNode));
	readyExistNode = ready_queue->front;
	int count = 0;
	long counter = 655350;
	static int printCtl = 0;
	if (enablePrinter != 0 && printCtl % enablePrinter == 0)
	{

		READ_MODIFY(MEMORY_INTERLOCK_BASE + 11, DO_LOCK, SUSPEND_UNTIL_LOCKED, &LockResultPrinter);
		printf("\n");
		//SP_print_header();
		SP_setup_action(SP_ACTION_MODE, action);

		if (currentPCB != NULL)
		{
			SP_setup(SP_RUNNING_MODE, currentPCB->pid);
		}
		else
		{
			SP_setup(SP_RUNNING_MODE, 99);
		}


		// print information of readyQueue, if the nodes in readyQueue are more than 10, just print the first 10
		
		while (readyExistNode->next != NULL)
		{
			readyExistNode = readyExistNode->next;
			count++;
			SP_setup(SP_READY_MODE, (readyExistNode->p_c_b).pid);
			if (count >= 10)
			{
				count = 0;
				break;
			}
		}

		// print information of timerQueue, if the nodes in timerQueue are more than 10, just print the first 10
		
		while (timerExistNode->next != NULL)
		{
			timerExistNode = timerExistNode->next;
			count++;
			SP_setup(SP_TIMER_SUSPENDED_MODE, (timerExistNode->p_c_b).pid);
			if (count >= 10)
			{
				count = 0;
				break;
			}
		}

		// print information of suspendQueue, if the nodes in suspendQueue are more than 10, just print the first 10

		while (suspendExistNode->next != NULL)
		{
			suspendExistNode = suspendExistNode->next;
			count++;
			SP_setup(SP_PROCESS_SUSPENDED_MODE, (suspendExistNode->p_c_b).pid);
			if (count >= 10)
			{
				count = 0;
				break;
			}
		}

		SP_print_line();
		// reset action to NULL
		memset(action, '\0', 8);
		printf("\n");

		READ_MODIFY(MEMORY_INTERLOCK_BASE + 11, DO_UNLOCK, SUSPEND_UNTIL_LOCKED, &LockResultPrinter);
	}
	else
	{
		while (counter > 0)
		{
			counter--;
		}
	}
	printCtl++;
}



/************************************************************************
SVC
The beginning of the OS502.  Used to receive software interrupts.
All system calls come to this point in the code and are to be
handled by the student written code here.
The variable do_print is designed to print out the data for the
incoming calls, but does so only for the first ten calls.  This
allows the user to see what's happening, but doesn't overwhelm
with the amount of data.
************************************************************************/

void    svc(SYSTEM_CALL_DATA *SystemCallData) {

	short               call_type;
	static INT16        do_print = 10;
	INT32               sleeptime, Time, Temp, Status;
	PCB					pcb;
	char				message[MAX_MSG_LENGTH];
	long				pid,error;
	int					terminate_status, suspend_status, resume_status, priority_status, send_message_status, receive_message_status;
	int msgLength;
	int returnStatus;
	short i;

	call_type = (short)SystemCallData->SystemCallNumber;
	if (do_print > 0) {
		printf("SVC handler: %s\n", call_names[call_type]);
		for (i = 0; i < SystemCallData->NumberOfArguments - 1; i++){
			printf("Arg %d: Contents = (Decimal) %8ld,  (Hex) %8lX\n", i,
				(unsigned long)SystemCallData->Argument[i],
				(unsigned long)SystemCallData->Argument[i]);
		}
		do_print--;
	}

	switch (call_type){
			// Get time service call
		case SYSNUM_GET_TIME_OF_DAY:     
			CALL(MEM_READ(Z502ClockStatus, &Time));
			*(INT32 *)SystemCallData->Argument[0] = Time;
			break;
			//terminate system call
		case SYSNUM_TERMINATE_PROCESS:
			if ((long)SystemCallData->Argument[0] == -2L)
			{
			//terminate itself and its children process 
				Z502Halt();
			}
			else if ((long)SystemCallData->Argument[0] == -1L)
			{
			//terminate itself, current PCB
				
				terminate_itself();
			}
			else
			{
				terminate_status = terminate_process(SystemCallData);
				*(INT32 *)SystemCallData->Argument[1] = terminate_status;
			}
			
			strncpy(action, "Teminat", 8);
			schedule_printer();
			break;
		case SYSNUM_SLEEP:
			sleeptime = (long)SystemCallData->Argument[0];
			start_timer(sleeptime);
			break;
		case SYSNUM_CREATE_PROCESS:
			pcb = create_process(SystemCallData);
			*(INT32 *)SystemCallData->Argument[3] = pcb.pid;
			*(INT32 *)SystemCallData->Argument[4] = pcb.error;
			strncpy(action, "Create", 8);
			schedule_printer();
			break;
		case SYSNUM_GET_PROCESS_ID:
			pid = get_process_id_by_name((char*)SystemCallData->Argument[0]);
			*(long*)SystemCallData->Argument[1] = pid;
			if (pid == -3)
			{
				*(long *)SystemCallData->Argument[2] = ERR_BAD_PARAM;
			}
			else
			{
				*(long *)SystemCallData->Argument[2] = ERR_SUCCESS;
			}
			break;
		case SYSNUM_SUSPEND_PROCESS:
			suspend_status = suspend_process(SystemCallData);
			*(long*)SystemCallData->Argument[1] = suspend_status;
			strncpy(action, "Suspend", 8);
			schedule_printer();
			break;
		case SYSNUM_RESUME_PROCESS:
			resume_status = resume_process(SystemCallData);
			*(long*)SystemCallData->Argument[1] = resume_status;
			strncpy(action, "Resume", 8);
			schedule_printer();
			break;
		case SYSNUM_CHANGE_PRIORITY:
			priority_status = change_priority_of_process(SystemCallData);
			*(long*)SystemCallData->Argument[2] = priority_status;
			strncpy(action, "ChgPior", 8);
			schedule_printer();
			break;
		case SYSNUM_SEND_MESSAGE:
			msgCount++;
			if (msgCount > 12)
			{
				*(long *)SystemCallData->Argument[3] = -1;
				break;
			}
			send_message_status = send_message(SystemCallData);
			*(long*)SystemCallData->Argument[3] = send_message_status;
			strncpy(action, "MsgSend", 8);
			schedule_printer();
			break;
		case SYSNUM_RECEIVE_MESSAGE:
			//receive_message_status = receive_message(SystemCallData);
			//*(long*)SystemCallData->Argument[5] = receive_message_status;
			pid = (long)SystemCallData->Argument[0];
			msgLength = (int)SystemCallData->Argument[2];
			// check msgLength
			if(msgLength > 100)
			{
				*(long *)SystemCallData->Argument[5] = ERR_BAD_PARAM;
			}
			else
			{
				returnStatus = receive_message_new(pid, (char*)SystemCallData->Argument[1], msgLength, SystemCallData->Argument[3], SystemCallData->Argument[4]);
				if (returnStatus == 0)
				{
					*(long *)SystemCallData->Argument[5] = ERR_SUCCESS;
				}
				else
				{
					*(long *)SystemCallData->Argument[5] = ERR_BAD_PARAM;
				}
			}
			break;
		default:
			printf("ERROR! Call_type not recognized!\n");
			printf("Call_type is - %i\n", call_type);
	}
}                                               // End of svc



/************************************************************************
osInit
This is the first routine called after the simulation begins.  This
is equivalent to boot code.  All the initial OS components can be
defined and initialized here.
************************************************************************/

void    osInit(int argc, char *argv[]) {
	void                *next_context;
	INT32               i;

	/* Demonstrates how calling arguments are passed thru to here       */

	printf("Program called with %d arguments:", argc);
	for (i = 0; i < argc; i++)
		printf(" %s", argv[i]);
	printf("\n");
	printf("Calling with argument 'sample' executes the sample program.\n");

	/*          Setup so handlers will come to code in base.c           */

	TO_VECTOR[TO_VECTOR_INT_HANDLER_ADDR] = (void *)interrupt_handler;
	TO_VECTOR[TO_VECTOR_FAULT_HANDLER_ADDR] = (void *)fault_handler;
	TO_VECTOR[TO_VECTOR_TRAP_HANDLER_ADDR] = (void *)svc;

	/*  Determine if the switch was set, and if so go to demo routine.  */

	if ((argc > 1) && (strcmp(argv[1], "sample") == 0)) {
		Z502MakeContext(&next_context, (void *)sample_code, KERNEL_MODE);
		Z502SwitchContext(SWITCH_CONTEXT_KILL_MODE, &next_context);
	}                   /* This routine should never return!!           */

	/*  This should be done by a "os_make_process" routine, so that
	test0 runs on a process recognized by the operating system.    */
	//Z502MakeContext(&next_context, (void *)test0, USER_MODE);
	//Z502SwitchContext(SWITCH_CONTEXT_KILL_MODE, &next_context);
	os_create_process(argc, argv);
}                                               // End of osInit


// ask the hardware for a context(Z502MakeContext), create the PCB, and then call Z502SwitchContext.
void	os_create_process(int argc, char *argv[]){
	void                *next_context=NULL;
	PCB					*rootPCB;
	QNode				*rootNode;

	rootPCB = malloc(sizeof(PCB));
	rootNode = malloc(sizeof(QNode));

	//create timer_queue
	timer_queue = malloc(sizeof(LQueue));
	timer_node = malloc(sizeof(QNode));
	//initial timer_queue
	timer_node->next = NULL;
	timer_queue->front = timer_queue->rear = timer_node;

	//create ready_queue
	ready_queue = malloc(sizeof(LQueue));
	ready_node = malloc(sizeof(QNode));
	//initial ready_queue
	ready_node->next = NULL;
	ready_queue->front = ready_queue->rear = ready_node;

	//create suspend_queue
	suspend_queue = malloc(sizeof(LQueue));
	suspend_node = malloc(sizeof(QNode));
	//initial suspend_queue
	suspend_node->next = NULL;
	suspend_queue->front = suspend_queue->rear = suspend_node;

	//create message_queue
	message_queue = malloc(sizeof(MQueue));
	message_node = malloc(sizeof(MNode));
	//initial message_queue
	message_node->next = NULL;
	message_queue->front = message_queue->rear = message_node;

	if ((argc > 1) && (strcmp(argv[1], "test0") == 0)){
		Z502MakeContext(&next_context, (void *)test0, USER_MODE);
	}
	else if((argc > 1) && (strcmp(argv[1], "test1a") == 0)){
		Z502MakeContext(&next_context, (void *)test1a, USER_MODE);
	}
	else if((argc > 1) && (strcmp(argv[1], "test1b") == 0)){
		Z502MakeContext(&next_context, (void *)test1b, USER_MODE);
	}
	else if((argc > 1) && (strcmp(argv[1], "test1c") == 0)){
		Z502MakeContext(&next_context, (void *)test1c, USER_MODE);
	}
	else if((argc > 1) && (strcmp(argv[1], "test1d") == 0)){
		Z502MakeContext(&next_context, (void *)test1d, USER_MODE);
	}
	else if((argc > 1) && (strcmp(argv[1], "test1e") == 0)){
		Z502MakeContext(&next_context, (void *)test1e, USER_MODE);
	}
	else if((argc > 1) && (strcmp(argv[1], "test1f") == 0)){
		Z502MakeContext(&next_context, (void *)test1f, USER_MODE);
	}
	else if((argc > 1) && (strcmp(argv[1], "test1g") == 0)){
		Z502MakeContext(&next_context, (void *)test1g, USER_MODE);
	}
	else if((argc > 1) && (strcmp(argv[1], "test1h") == 0)){
		Z502MakeContext(&next_context, (void *)test1h, USER_MODE);
	}
	else if((argc > 1) && (strcmp(argv[1], "test1i") == 0)){
		Z502MakeContext(&next_context, (void *)test1i, USER_MODE);
	}
	else if((argc > 1) && (strcmp(argv[1], "test1j") == 0)){
		Z502MakeContext(&next_context, (void *)test1j, USER_MODE);
	}
	else if((argc > 1) && (strcmp(argv[1], "test1k") == 0)){
		Z502MakeContext(&next_context, (void *)test1k, USER_MODE);
	}
	else if((argc > 1) && (strcmp(argv[1], "test1l") == 0)){
		Z502MakeContext(&next_context, (void *)test1l, USER_MODE);
	}
	else if((argc > 1) && (strcmp(argv[1], "test1m") == 0)){
		Z502MakeContext(&next_context, (void *)test1m, USER_MODE);
	}
	
	//Z502MakeContext(&next_context, (void *)test1h, USER_MODE);
	//create the rootPCB
	rootPCB->pid = ROOT_PID;
	strcpy(rootPCB->name, "ROOT");
	rootPCB->context = next_context;
	rootPCB->priority = ROOT_PRIORITY;
	rootNode->p_c_b = *rootPCB;
	rootNode->next = NULL;
	currentPCB = rootPCB;
	
	Z502SwitchContext(SWITCH_CONTEXT_KILL_MODE, &next_context);
}
