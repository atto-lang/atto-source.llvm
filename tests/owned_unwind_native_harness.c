#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
typedef int64_t I;
#define ARGS I*,I,I
extern I unwind_words_v1(void),unwind_arity_v1(void);
extern I unwind_start_v1(ARGS),unwind_pending_v1(ARGS),unwind_resume_v1(ARGS),unwind_outcome_v1(ARGS),unwind_error_v1(ARGS),unwind_suppressed_count_v1(ARGS),unwind_retire_v1(ARGS);
extern I unwind_complete_v1(ARGS,I,I,I),unwind_result_v1(ARGS,I*),unwind_suppressed_error_v1(ARGS,I),unwind_suppressed_rank_v1(ARGS,I);
#define C(n) unwind_##n##_v1(f,7,1)
static void wake(I *f,I slot,I kind,I value,I next){
 assert(C(pending)==slot);assert(C(resume)==-1);assert(C(retire)==-1);
 assert(unwind_complete_v1(f,8,1,slot,kind,value)==-1);
 assert(unwind_complete_v1(f,7,2,slot,kind,value)==-1);
 assert(unwind_complete_v1(f,7,1,slot==1?2:1,kind,value)==-1);
 assert(unwind_complete_v1(f,7,1,slot,3,value)==-1);
 assert(unwind_complete_v1(f,7,1,slot,1,0)==-1);
 assert(unwind_complete_v1(f,7,1,slot,kind,value)==0);
 assert(unwind_complete_v1(f,7,1,slot,kind,value)==-1);
 assert(C(pending)==-1);I out=999;assert(unwind_result_v1(f,7,1,&out)==-1&&out==999);
 assert(C(resume)==next);
}
static void finish(I *f,I kind,I value,I err,I suppressed,I sr){
 assert(C(outcome)==kind);I out=999;
 if(kind<=2)assert(unwind_result_v1(f,7,1,&out)==0&&out==value);else assert(unwind_result_v1(f,7,1,&out)==-1&&out==999);
 assert(C(error)==err);assert(C(suppressed_count)==(suppressed!=0));
 if(suppressed){assert(unwind_suppressed_error_v1(f,7,1,0)==suppressed);assert(unwind_suppressed_rank_v1(f,7,1,0)==sr);}
 assert(unwind_suppressed_error_v1(f,7,1,-1)==-1);
 assert(C(retire)==0&&C(retire)==-1&&C(start)==-1);
 assert(C(outcome)==-1&&unwind_complete_v1(f,7,1,1,0,99)==-1);
 for(int i=0;i<26;i++)assert(f[i]==(i==2?33:0));
}
int main(int argc,char **argv){
 assert(argc==2&&unwind_words_v1()==26);
 I a[26]={0},b[26]={0};
 assert(unwind_start_v1(NULL,7,1)==-1);
 assert(unwind_start_v1(a,0,1)==-1);
 assert(unwind_start_v1(a,7,1)==1&&unwind_start_v1(a,7,1)==-1);
 assert(unwind_start_v1(b,7,1)==1);
 if(!strcmp(argv[1],"defer")){
  assert(unwind_arity_v1()==2);
  wake(a,2,0,7,1);assert(unwind_pending_v1(b,7,1)==2);
  wake(b,2,2,11,1);wake(a,1,0,9,2);finish(a,1,42,0,0,0);
  wake(b,1,1,22,2);finish(b,5,0,11,22,2);
 }else{
  assert(unwind_arity_v1()==3);
  wake(a,1,1,11,1);assert(unwind_pending_v1(b,7,1)==1);
  wake(b,1,0,-42,1);wake(a,2,0,0,1);wake(b,2,0,0,2);finish(b,1,-42,0,0,0);
  wake(a,3,0,42,2);finish(a,2,42,11,0,0);
  for(int k=0;k<3;k++){
   I f[26]={0};assert(C(start)==1);
   if(k==0){wake(f,1,2,11,1);wake(f,2,0,0,2);finish(f,5,0,11,0,0);}
   if(k==1){wake(f,1,1,11,1);wake(f,2,1,22,2);finish(f,4,0,22,11,1);}
   if(k==2){wake(f,1,1,11,1);wake(f,2,0,0,1);wake(f,3,1,33,2);finish(f,4,0,33,11,1);}
  }
 }
 puts("PASS_NATIVE_OWNED_UNWIND");return 0;
}
