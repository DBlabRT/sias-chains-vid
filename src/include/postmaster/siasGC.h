


typedef struct RequestQueue
{
	RelFileNode elements[SIAS_MAX_RELATIONS];
	int head;		/* index into queue pointing to the earliest inserted element */
	int tail;		/* index into queue pointing to the latest inserted element */
	int numElements;
	slock_t mutex;
} RequestQueue;

typedef struct SiasGcShmemStruct
{
	RequestQueue requestQueue;
	uint32 numWorkers;
	slock_t numWorkersMutex;
	bool needRebuild;
} SiasGcShmemStruct;


extern int StartSiasGcWorker(void);

extern bool RequestGC(Relation rel, BlockNumber firstBlock);

extern void SetGcRunDurationFileDir(char *dataDir);

extern void SiasGcShmemInit(void);

extern bool IsSiasGcWorkerProcess(void);
