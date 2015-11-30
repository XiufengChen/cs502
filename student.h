#include <stdio.h>
#include <stdlib.h>

#define				ROOT_PID					0;
#define				ROOT_PRIORITY				20;
#define				ROOT_NAME					"ROOT"
/*****************************************************************/
#define             ILLEGAL_PRIORITY            -3
#define             LEGAL_PRIORITY              10
/*****************************************************************/
#define             DO_LOCK                     1
#define             DO_UNLOCK                   0
/*****************************************************************/
#define             SUSPEND_UNTIL_LOCKED        TRUE
#define             DO_NOT_SUSPEND              FALSE
/*****************************************************************/
#define				MAX_MSG_LENGTH			    100	
/*****************************************************************/
typedef struct
{
	long	pid;
	char	name[100];
	void	*context;
	int		priority;
	INT32   wakeuptime;
	int		error;
} PCB;

typedef struct
{
	long sid;
	long tid;
	INT32 length;
	char msg[MAX_MSG_LENGTH];
} MSG;



/*****************************************************************/

typedef struct node
{
	PCB p_c_b;
	struct node *next;
}   QNode;

typedef struct queue
{
	QNode *front;
	QNode *rear;
}	LQueue;

typedef struct mnode
{
	MSG m_s_g;
	struct mnode *next;
}   MNode;

typedef struct mqueue
{
	MNode *front;
	MNode *rear;
}	MQueue;

/*****************************************************************/

LQueue *Init_LQueue()
{
	LQueue *q;
	QNode *n;
	q = malloc(sizeof(LQueue)); 
	n = malloc(sizeof(QNode)); 
	n->next = NULL; q->front = q->rear = n;
	return q;
}

void In_LQueue(LQueue *q, QNode *n)
{
	q->rear->next = n;
	q->rear = n;
}

int Empty_LQueue(LQueue *q)
{
	if (q->front == q->rear) return 0;
	else return 1;
}

PCB Out_LQueue(LQueue *q)
{
		QNode *n;
		PCB *x;
		x = malloc(sizeof(PCB));
		n = q->front->next;
		q->front->next = n->next;
		*x = n->p_c_b;
		free(n);
		if (q->front->next == NULL)
			q->rear = q->front;
		return *x;
}
