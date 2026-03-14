#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/xattr.h>


#if __x86_64__
   typedef long s64;
   typedef unsigned long u64;
#else
   typedef long long s64;
   typedef unsigned long long u64;
#endif

#define MAX_RANGES_SUPPORTED 	4
#define BLOCK_SIZE		4096	//4 KB block
#define BLOCK_SIZE_BITS		12	//4 KB block

typedef enum{
               SNAPX_FLAG_COW,
	       SNAPX_FLAG_COW_RO,  //XXX Does it make sense in some use-case?
               SNAPX_FLAG_SEE_TH,    
               SNAPX_FLAG_SEE_TH_RO,    
               SNAPX_FLAG_SHARED,
               SNAPX_FLAG_SHARED_RO, //XXX Does it make sense in some use-case?
	       SNAPX_FLAG_MAX
}WriteCh;	

char *WriteBNames[SNAPX_FLAG_MAX] = {"cow", "cow-ro", "seeth", "seeth-ro", "shr", "shr-ro"}; 
//range is inclusive of start and end
//Block numbers start from 0
struct child_range
{
	s64 start;	            //start block num
	s64 end;		    //end block num
	WriteCh snapx_behavior;     //control flags to decide write behavior 
};

struct helper_inodes
{
	u64 p_inode;	//parent's inode
	u64 f_inode;	//friend's inode
	u64 l_inode;    //log's inode
	int num_ranges;		//how many ranges to store corresponding a child?
	struct child_range range[MAX_RANGES_SUPPORTED];	//maintain static snapshot of these blocks
};

static void usage_exit(char *cmd)
{
  printf("Usage: %s -c|-C <childfile> -p|-P <parentfile> -f|-F <friendfile> -l|-L <logfile> -r|R <comma,separated,ranges>\n", cmd);
  printf("How to pass range information?\n");
  printf("\t Format {start_block:end_block:cow_behavior},{start_block:end_block:cow_behavior} ...\n");
  printf("\t The block ranges are inclusive of start and end and should be in non-overlapping sorted order.\n");
  printf("\t CoW behavior\n");
  printf("\t\t 0: Default CoW\n");
  printf("\t\t 1: Default CoW but read-only permission for this file\n");
  printf("\t\t 2: See through. All updates of parent visible\n");
  printf("\t\t 3: See through with read-only permission for this file. All updates visible, can not write\n");
  printf("\t\t 4: Shared. All updates visible to every peer\n");
  printf("\t\t 5: Shared with read-only permission for this file.\n");
  printf("\t Maximum supported ranges: %d\n", MAX_RANGES_SUPPORTED);
  printf("\n");
  exit(-1);
}

static int parse_and_fill_range_info(char *rstr, long num_par_blks, struct helper_inodes *hino)
{
   char *range[MAX_RANGES_SUPPORTED];
   char *tokptr;
   int num_ranges = 0;
   do{
         tokptr = strtok(rstr, ",");
	 if(!tokptr)
		 break;
	 sscanf(tokptr, "%ld:%ld:%d", &hino->range[num_ranges].start, &hino->range[num_ranges].end, (int *)(&hino->range[num_ranges].snapx_behavior));
         if((!rstr && hino->range[num_ranges].start <= hino->range[num_ranges-1].end)
		  || hino->range[num_ranges].end >= num_par_blks
		  || hino->range[num_ranges].start < 0
		  || hino->range[num_ranges].snapx_behavior < 0
		  || hino->range[num_ranges].snapx_behavior >= SNAPX_FLAG_MAX)
		return -1;
	 ++num_ranges;
	 rstr = NULL;
   }while(tokptr);
   return num_ranges;
}
//Returns fd of the created child file
static int parse_get_child(int argc, char **argv, struct helper_inodes *hino)
{
  char *childfname = NULL;
  char *logfname = NULL;
  char *parentfname = NULL;
  char *frndfname = NULL;
  char *rstr = NULL;
  int pfd, cfd, ffd , lfd;
  struct stat statbuf;
  s64 num_par_blks = 0;
  while(argc > 0){
	   char *ptr = *argv;
	   if(*ptr != '-')
		  return -1;
	   ptr++;
           switch(*ptr){
		   case 'c':
		   case 'C':     	
			      argv++;
			      argc--;
			      if(argc <= 0)
				      return -1;
			      childfname = *argv;
			      break;
	           case 'p':		      
	           case 'P':		      
			      argv++;
			      argc--;
			      if(argc <= 0)
				      return -1;
			      parentfname = *argv;
			      break;
	           case 'f':
	           case 'F':
			      argv++;
			      argc--;
			      if(argc <= 0)
				      return -1;
			      frndfname = *argv;
			      break;
		   case 'l':
		   case 'L':
			      argv++;
		 	      argc--;
			      if(argc <= 0)
				      return -1;
			      logfname = *argv;
			      break;	
	           case 'r':
	           case 'R':
			      argv++;
			      argc--;
			      if(argc <= 0)
				      return -1;
			      rstr = *argv;
			      break;
		   default:
		             printf("Unknown option\n");
	                     return -1;		     

	   }
	 argv++;
         argc--;	 
  }  
  
  if(!childfname){
	  printf("Child file name is not passed\n");
	  return -1;
  }
  
  if(!parentfname){
	  printf("Parent file name is not passed\n");
	  return -1;
  }
  
  if(!frndfname){
	  printf("Friend file name is not passed\n");
	  return -1;
  }

  if(!logfname){
	  printf("Log file name is not passed\n");
	  return -1;
  }
  pfd = open(parentfname, O_RDWR);
  if(pfd < 0){
	  perror("parent file open");
	  return -1;
  }
  
  lfd = open(logfname, O_RDWR | O_CREAT , 0644);
  if(lfd < 0){
	  perror("log file open");
	  return -1;
  }
	
  fstat(pfd, &statbuf);
  hino->p_inode = statbuf.st_ino;
  num_par_blks = (((statbuf.st_size) % BLOCK_SIZE) == 0) ? (statbuf.st_size >> BLOCK_SIZE_BITS) : (1 + (statbuf.st_size >> BLOCK_SIZE_BITS));
  
  fstat(lfd, &statbuf);
  hino->l_inode = statbuf.st_ino;
  printf("Passing hino->l_inode = %d\n" , hino->l_inode);  
  

if(!num_par_blks){
	  printf("Zero sized parent file\n");
	  return -1;
  }
  if(!rstr){  //No range information, default CoW
	hino->num_ranges = 0;
  }else{
	 hino->num_ranges = parse_and_fill_range_info(rstr, num_par_blks, hino);
         if(hino->num_ranges < 0){
	       printf("Badly formed range information\n");
	       return -1;
	 }
  }  
   
  printf("Info passed pinode:%lx par_blocks: %ld num_ranges:%d\n", hino->p_inode, num_par_blks, hino->num_ranges);
  
  printf("---Snapx range settings---\n");
  if(!hino->num_ranges)
    	      printf("[0 - %ld]:%s\n", num_par_blks-1, WriteBNames[0]);
  for(int i=0; i<hino->num_ranges; ++i){
          if(i == 0 && hino->range[i].start != 0){
    	      printf("[0 - %ld]:%s\n", hino->range[i].start-1, WriteBNames[0]);
	  }else if(i && hino->range[i].start != hino->range[i-1].end + 1){
    	        printf("[%ld - %ld]:%s\n", hino->range[i-1].end + 1, hino->range[i].start - 1, WriteBNames[0]);
	  }
    	  printf("[%ld - %ld]:%s\n", hino->range[i].start, hino->range[i].end, WriteBNames[hino->range[i].snapx_behavior]);
	  if(i == hino->num_ranges -1 && hino->range[i].end + 1 != num_par_blks)
    	          printf("[%ld - %ld]:%s\n", hino->range[i].end+1, num_par_blks - 1, WriteBNames[0]);
  }
  //Now the rest of the stuff. Create friend and child etc.  
  cfd = open(childfname, O_RDWR | O_CREAT, 0644);
  if(cfd < 0){
	  perror("child file open");
	  return -1;
  }
  
  ffd = open(frndfname, O_RDWR | O_CREAT, 0644);
  if(ffd < 0){
	  perror("friend file open");
	  close(pfd);
	  close(cfd);
	  unlink(childfname);
	  return -1;
  } 
  fstat(ffd, &statbuf);
  hino->f_inode = statbuf.st_ino;
  close(pfd);
  close(ffd);
  close(lfd);  
  return cfd;  
}

int main(int argc, char* argv[])
{
	struct helper_inodes h_inodes;
	char *attr_name = "user.SCORW_PARENT";
	int ret, cfd;

        cfd = parse_get_child(argc-1, argv+1, &h_inodes);
        if(cfd < 0)
		usage_exit(argv[0]);
	
	ret = fsetxattr(cfd, attr_name , &h_inodes, sizeof(struct helper_inodes), 0);
	if(ret == -1)
	{
		perror("setxattr failed");
		return -1;
	}
        close(cfd);

	//This call is required to initialize the frnd file
	//Update: This call is not required now. Frnd file gets initialised
	//	during the setting of extended attributes itself
	//Update:
	//	This call is required. I think, code that attaches child to an already open parent file assumes this open is called.
	//	Fix that code before removing this open call.
        cfd = open(argv[1], O_RDWR, 0644);

	return 0;
}
