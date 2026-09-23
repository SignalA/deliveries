#define _GNU_SOURCE
#include "calibration_hid.h"
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

uint16_t calib_crc16(const unsigned char *data, size_t length)
{
    uint16_t crc=0xffff;
    for(size_t i=0;i<length;++i) {
        crc^=data[i];
        for(int bit=0;bit<8;++bit) crc=(crc&1)?(uint16_t)((crc>>1)^0xa001):(uint16_t)(crc>>1);
    }
    return crc;
}
static void put32(unsigned char *p,int value)
{
    p[0]=(unsigned char)value; p[1]=(unsigned char)(value>>8);
    p[2]=(unsigned char)(value>>16); p[3]=(unsigned char)(value>>24);
}
static void build_command(unsigned char report[65],unsigned func,unsigned index,
                          const unsigned char *data,size_t length)
{
    memset(report,0,65);
    unsigned char *p=report+1;
    p[0]=(unsigned char)func; p[1]=(unsigned char)length; put32(p+2,(int)index);
    if(length)memcpy(p+6,data,length);
    uint16_t crc=calib_crc16(p,6+length);
    p[6+length]=(unsigned char)crc; p[7+length]=(unsigned char)(crc>>8);
}
void calib_build_profile_report(unsigned char report[65],int profile)
{
    unsigned char selected=(unsigned char)profile;
    build_command(report,0x05,7,&selected,1);
}
void calib_build_status_report(unsigned char report[65])
{
    build_command(report,0x05,8,NULL,0);
}
void calib_build_report(unsigned char report[65], int point,
                        const int actual_xy[6], int target_x, int target_y)
{
    unsigned char data[33]={0}; data[0]=(unsigned char)point;
    int pos=1;
    for(int i=0;i<6;++i) { put32(data+pos,actual_xy[i]); pos+=4; }
    put32(data+pos,target_x); pos+=4; put32(data+pos,target_y);
    build_command(report,0x05,6,data,sizeof(data));
}
static int parse_frame(const unsigned char *report,size_t length,unsigned *func,
                       unsigned *index,const unsigned char **data,size_t *data_length)
{
    if(!report || !length) return 0;
    size_t start=report[0]==0?1:0;
    if(length-start<9) return 0;
    const unsigned char *p=report+start;
    size_t body_length=(size_t)6+p[1];
    if(body_length+2>length-start) return 0;
    uint16_t got=(uint16_t)(p[body_length]|(p[body_length+1]<<8));
    if(got!=calib_crc16(p,body_length)) return 0;
    if(func)*func=p[0];
    if(index)*index=(unsigned)p[2]|((unsigned)p[3]<<8)|((unsigned)p[4]<<16)|((unsigned)p[5]<<24);
    if(data)*data=p+6;
    if(data_length)*data_length=p[1];
    return 1;
}
int calib_parse_status(const unsigned char *report,size_t length,int *active_profile,
                       unsigned *valid_mask,int *last_result,int *last_profile)
{
    unsigned func,index; const unsigned char *data; size_t n;
    if(!parse_frame(report,length,&func,&index,&data,&n) || func!=0x05 || index!=8 || n!=4)return 0;
    if(active_profile)*active_profile=data[0];
    if(valid_mask)*valid_mask=data[1];
    if(last_result)*last_result=data[2];
    if(last_profile)*last_profile=data[3];
    return 1;
}
int calib_parse_response(const unsigned char *report,size_t length,int *final_result)
{
    unsigned func,index; const unsigned char *data; size_t n;
    if(!parse_frame(report,length,&func,&index,&data,&n) || index!=6 || n!=1)return 0;
    if(func==0x0a) { if(final_result)*final_result=data[0]==0?1:-1; return 1; }
    if(func==0x05 && data[0]==1) { if(final_result)*final_result=0; return 1; }
    return 0;
}
static int read_value(const char *path,const char *key,char *out,size_t size)
{
    FILE *f=fopen(path,"r"); if(!f)return 0;
    char line[512]; int found=0;
    while(fgets(line,sizeof(line),f)) {
        char *eq=strchr(line,'='); if(!eq)continue; *eq++=0;
        eq[strcspn(eq,"\r\n")]=0;
        if(!strcmp(line,key)) { snprintf(out,size,"%s",eq); found=1; break; }
    }
    fclose(f); return found;
}
static int custom_match(const char *sys,const char *physical,const char *unique)
{
    char uevent[640],id[128]={0},phys[256]={0},uniq[128]={0};
    snprintf(uevent,sizeof(uevent),"%s/device/uevent",sys);
    if(!read_value(uevent,"HID_ID",id,sizeof(id)) ||
       !strstr(id,"00002E2C:00000631") || !read_value(uevent,"HID_PHYS",phys,sizeof(phys))) return 0;
    size_t n=strlen(phys); if(n<7 || strcmp(phys+n-7,"/input2")) return 0;
    phys[n-7]=0;
    if(physical && *physical) return !strcmp(phys,physical);
    return unique && *unique && read_value(uevent,"HID_UNIQ",uniq,sizeof(uniq)) && !strcmp(uniq,unique);
}
static int open_command(const char *physical,const char *unique,char *path,size_t path_size)
{
    glob_t matches={0};
    if(glob("/sys/class/hidraw/hidraw*",0,NULL,&matches)) return -1;
    int fd=-1;
    for(size_t i=0;i<matches.gl_pathc;++i) if(custom_match(matches.gl_pathv[i],physical,unique)) {
        const char *name=strrchr(matches.gl_pathv[i],'/');
        if(!name)continue;
        snprintf(path,path_size,"/dev/%s",name+1);
        fd=open(path,O_RDWR|O_NONBLOCK|O_CLOEXEC);
        if(fd>=0)break;
    }
    globfree(&matches); return fd;
}
static int wait_frame(int fd,int timeout_ms,unsigned want_func,unsigned want_index,
                      unsigned char *data,size_t *data_length)
{
    struct pollfd p={fd,POLLIN,0};
    for(;;) {
        int ready; do ready=poll(&p,1,timeout_ms); while(ready<0 && errno==EINTR);
        if(ready<=0)return 0;
        unsigned char report[65]; ssize_t bytes=read(fd,report,sizeof(report));
        if(bytes>0 && getenv("GAIME_HID_TRACE")) {
            fprintf(stderr,"HID_RX bytes=%zd data=",bytes);
            for(ssize_t i=0;i<bytes;++i)fprintf(stderr,"%02X",report[i]);
            fputc('\n',stderr);
        }
        unsigned func,index; const unsigned char *payload; size_t n;
        if(bytes>0 && parse_frame(report,(size_t)bytes,&func,&index,&payload,&n) &&
           func==want_func && index==want_index) {
            if(data && n)memcpy(data,payload,n);
            if(data_length)*data_length=n;
            return 1;
        }
    }
}
static int send_report(int fd,const unsigned char report[65])
{
    ssize_t n; do n=write(fd,report,65); while(n<0 && errno==EINTR);
    if(getenv("GAIME_HID_TRACE")) {
        fprintf(stderr,"HID_TX bytes=%zd data=",n);
        for(size_t i=0;i<65;++i)fprintf(stderr,"%02X",report[i]);
        if(n<0)fprintf(stderr," error=%s",strerror(errno));
        fputc('\n',stderr);
    }
    return n==65;
}
static int select_profile_on_fd(int fd,int profile)
{
    unsigned char report[65],response[8]; size_t response_length=0;
    calib_build_profile_report(report,profile);
    if(!send_report(fd,report))return CALIB_HID_WRITE;
    if(!wait_frame(fd,700,0x05,7,response,&response_length))return CALIB_HID_NO_RESPONSE;
    return response_length==1 && response[0]==1 ? CALIB_HID_OK : CALIB_HID_REJECTED;
}
int calib_select_profile(const char *physical,const char *unique,int profile,
                         char *device_path,size_t path_size)
{
    if(profile<0 || profile>3)return CALIB_HID_REJECTED;
    int fd=open_command(physical,unique,device_path,path_size);
    if(fd<0)return CALIB_HID_DEVICE;
    int result=select_profile_on_fd(fd,profile);
    if(result==CALIB_HID_OK) {
        unsigned char report[65],response[8]; size_t response_length=0;
        calib_build_status_report(report);
        if(!send_report(fd,report))result=CALIB_HID_WRITE;
        else if(!wait_frame(fd,700,0x05,8,response,&response_length))result=CALIB_HID_NO_RESPONSE;
        else if(response_length!=4 || response[0]!=(unsigned char)profile)result=CALIB_HID_REJECTED;
    }
    close(fd);
    return result;
}
int calib_transmit(const char *physical,const char *unique,int profile,const int coords[48],const int targets[16],char *device_path,size_t path_size)
{
    if(profile<0 || profile>3)return CALIB_HID_REJECTED;
    int fd=open_command(physical,unique,device_path,path_size); if(fd<0)return CALIB_HID_DEVICE;
    static const int tx[8]={5616,16383,27150,5616,27150,5616,16383,27150};
    static const int ty[8]={5406,5406,5406,16383,16383,27360,27360,27360};
    unsigned char report[65],response[8]; size_t response_length=0;

    int selected=select_profile_on_fd(fd,profile);
    if(selected!=CALIB_HID_OK) { close(fd); return selected; }

    for(int point=0;point<8;++point) {
        int actual[6];
        for(int shot=0;shot<3;++shot) { actual[shot*2]=coords[(point*3+shot)*2]; actual[shot*2+1]=coords[(point*3+shot)*2+1]; }
        int target_x=targets?targets[point*2]:tx[point];
        int target_y=targets?targets[point*2+1]:ty[point];
        calib_build_report(report,point,actual,target_x,target_y);
        if(!send_report(fd,report)) { close(fd); return CALIB_HID_WRITE; }
    }
    if(!wait_frame(fd,1000,0x0a,6,response,&response_length)) { close(fd); return CALIB_HID_NO_RESPONSE; }
    if(response_length!=1 || response[0]!=0) { close(fd); return CALIB_HID_REJECTED; }

    for(int attempt=0;attempt<10;++attempt) {
        usleep(30000);
        calib_build_status_report(report);
        if(!send_report(fd,report)) { close(fd); return CALIB_HID_WRITE; }
        if(!wait_frame(fd,300,0x05,8,response,&response_length))continue;
        if(response_length!=4) { close(fd); return CALIB_HID_REJECTED; }
        unsigned mask=response[1],result=response[2],last_profile=response[3];
        if(result==0 && last_profile==(unsigned)profile && (mask&(1u<<profile))) { close(fd); return CALIB_HID_OK; }
        if(last_profile==(unsigned)profile && (result==1 || result==2)) { close(fd); return CALIB_HID_REJECTED; }
    }
    close(fd); return CALIB_HID_NO_RESPONSE;
}
