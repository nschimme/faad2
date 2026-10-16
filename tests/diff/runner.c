/* Same source is compiled separately against the shim and stock FAAD2. */
#include <neaacdec.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t crc(uint32_t c, const unsigned char *p, size_t n) {
    size_t i; unsigned b;
    for(i=0;i<n;i++) { c ^= p[i]; for(b=0;b<8;b++) c=(c>>1)^((0u-(c&1))&0xedb88320u); } return c;
}
static uint32_t le(const unsigned char *p) { return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
int main(int argc,char **argv) {
    FILE *f,*pcm; long len; unsigned char *data,asc[2],*config;
    size_t n,pos=0,index=0,cs; unsigned long sr=0; unsigned char ch=0;
    int raw,fmt,dab; long status; uint32_t sum=~0u;
    NeAACDecHandle h; NeAACDecConfigurationPtr cfg;
    if(argc!=5) return 2;
    raw=atoi(argv[2]); fmt=atoi(argv[3]); dab=strstr(argv[1],".dab")!=NULL;
    f=fopen(argv[1],"rb"); if(!f) return 2;
    fseek(f,0,SEEK_END);len=ftell(f);rewind(f);if(len<7)return 2;
    n=(size_t)len;data=calloc(n+64,1);if(!data)return 2;
    if(fread(data,1,n,f)!=n)return 2;fclose(f);
    pcm=fopen(argv[4],"wb");if(!pcm)return 2;
    h=NeAACDecOpen();if(!h)return 2;cfg=NeAACDecGetCurrentConfiguration(h);cfg->outputFormat=fmt;
    if(!NeAACDecSetConfiguration(h,cfg))return 2;
    if(dab) {cs=le(data);if(cs>n-4)return 2;config=data+4;pos=4+cs;}
    else {unsigned sfi=(data[2]>>2)&15,cc=((data[2]&1)<<2)|(data[3]>>6);asc[0]=16|(sfi>>1);asc[1]=((sfi&1)<<7)|(cc<<3);config=asc;cs=2;}
    if(raw||dab) {
        mp4AudioSpecificConfig a; int ret=(signed char)NeAACDecAudioSpecificConfig(config,cs,&a);
        printf("{\"kind\":\"asc\",\"status\":%d,\"object\":%u}\n",ret,a.objectTypeIndex);
        status=(signed char)NeAACDecInit2(h,config,cs,&sr,&ch);
    } else status=NeAACDecInit(h,data,n,&sr,&ch);
    printf("{\"kind\":\"init\",\"status\":%ld,\"rate\":%lu,\"channels\":%u}\n",status,sr,ch);
    if(status<0)return 1;
    while(pos<n) {
        size_t size,skip=0;NeAACDecFrameInfo fi;void *p;
        if(dab) {if(n-pos<4)return 2;size=le(data+pos);pos+=4;}
        else {if(n-pos<7)return 2;size=((data[pos+3]&3)<<11)|(data[pos+4]<<3)|(data[pos+5]>>5);if(raw)skip=(data[pos+1]&1)?7:9;}
        if(size<skip||size>n-pos||!size)return 2;
        p=NeAACDecDecode(h,&fi,data+pos+skip,size-skip);
        printf("{\"kind\":\"frame\",\"index\":%zu,\"bytesconsumed\":%lu,\"samples\":%lu,\"channels\":%u,\"samplerate\":%lu,\"sbr\":%u,\"ps\":%u,\"object_type\":%u,\"header_type\":%u,\"error\":%u}\n",index++,fi.bytesconsumed,fi.samples,fi.channels,fi.samplerate,fi.sbr,fi.ps,fi.object_type,fi.header_type,fi.error);
        if(fi.error||(!p&&fi.samples))return 1;
        if(fi.samples) {size_t bytes=fi.samples*(fmt==1?2:4);sum=crc(sum,p,bytes);if(fwrite(p,1,bytes,pcm)!=bytes)return 2;}
        pos+=size;
    }
    printf("{\"kind\":\"crc\",\"value\":\"%08x\"}\n",~sum);
    fclose(pcm);NeAACDecClose(h);free(data);return 0;
}
