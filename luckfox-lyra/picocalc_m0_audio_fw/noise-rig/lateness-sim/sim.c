/* 128-cycle tick, second-order modulator as in play.S (silence), pins on a 37.5-cycle grid.
 * usage: sim2 N p hmax stall pstall mode x_true x_est seed
 *  issue lateness: lag (catch-up 14/tick, p chance of +2..hmax before the write)
 *  stall inside the write: extra blocking pstall chance of 1..stall cycles (does NOT move the pin)
 *  mode 0 none | 1 post-write count (return time) | 2 issue time | 3 issue time + grid model (x_est) */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
static uint64_t s=88172645463325252ULL;
static double rnd(){ s^=s<<13; s^=s>>7; s^=s<<17; return (s>>11)*(1.0/9007199254740992.0); }
#define G 37.5
static double grid(double t,double x){ return G*ceil((t+x)/G); }
int main(int argc,char**argv){
  long N=atol(argv[1]); double p=atof(argv[2]); int hmax=atoi(argv[3]); int stall=atoi(argv[4]); double pst=atof(argv[5]);
  int mode=atoi(argv[6]); double xt=atof(argv[7]), xe=atof(argv[8]); s^=atol(argv[9])*0x9E3779B97F4A7C15ULL;
  double i1=0,i2=0,g=1.0; int level=1; double lag=0,prev_issue=0,prev_post=0,prev_q=0,prev_pin=0;
  float*A=malloc(N*sizeof(float)); double nominal=0, last_pin=0; double stallprev=0;
  for(long n=0;n<N;n++){
    nominal=n*128.0;
    /* issue time: tick time + lateness; lateness follows the previous write's stall less catch-up */
    lag-=14; if(lag<0)lag=0; lag+=stallprev; stallprev=0;
    if(rnd()<p) lag+=2+(int)(rnd()*(hmax-1));
    double issue=nominal+lag;
    double st=(rnd()<pst)?1+(int)(rnd()*stall):0;      /* extra blocking inside this write */
    stallprev=st;
    double ret=issue+53+st;                             /* write returns */
    double pin=grid(issue,xt);                          /* the pin follows issue time on the 10 MHz grid */
    double interval=(n==0)?128:pin-last_pin;            /* what the level that just ended really lasted */
    double meas;
    if(mode==0) meas=128;
    else if(mode==1) meas=(n==0)?128:ret-prev_post;
    else if(mode==2) meas=(n==0)?128:issue-prev_issue;
    else meas=(n==0)?128:grid(issue,xe)-prev_q;
    /* firmware window on what it measures */
    double d=meas-128; d=fmod(d+28.0+128.0*64,128.0)-28.0; meas=128+d;
    A[n]=level*interval/128.0; last_pin=pin;
    double corr=level*(meas-128)/128.0;
    i1-=corr; i2-=corr;
    i1+=0.0-g; double y=i2+i1; level=(y>=0)?1:-1; g=level; i2=y-g;
    prev_post=ret; prev_issue=issue; prev_q=grid(issue,xe);
  }
  FILE*f=fopen("area.f32","wb"); fwrite(A,sizeof(float),N,f); fclose(f);
  return 0;
}
