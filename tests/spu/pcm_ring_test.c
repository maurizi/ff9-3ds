#include <assert.h>
#include <stdio.h>
#include "pcm_ring.h"
static PortPcmRing ring;
static int16_t input[2048], output[2048];
static int16_t value(uint32_t index,unsigned channel) { return (int16_t)(index * 2357u + channel * 19217u); }
int main(void) {
    port_pcm_reset(&ring);
    /* Start close to 32-bit wrap, covering >27-hour stream cursor rollover. */
    ring.written=ring.read=UINT32_MAX-500;
    uint32_t written=ring.written,read=ring.read,random=1,underrun=0;
    for(unsigned step=0;step<20000;++step) {
        random=random*1664525u+1013904223u;
        unsigned request=(random>>16)%1025;
        if(random&1) {
            for(unsigned i=0;i<request;++i)for(unsigned ch=0;ch<2;++ch)input[i*2+ch]=value(written+i,ch);
            unsigned expected=PORT_PCM_CAPACITY-(written-read);if(expected>request)expected=request;
            assert(port_pcm_push(&ring,input,request)==expected);written+=expected;
        } else {
            unsigned expected=written-read;if(expected>request)expected=request;
            assert(port_pcm_pull(&ring,output,request)==expected);
            for(unsigned i=0;i<request;++i)for(unsigned ch=0;ch<2;++ch)
                assert(output[i*2+ch]==(i<expected?value(read+i,ch):0));
            read+=expected;underrun+=request-expected;
        }
        assert(ring.written==written&&ring.read==read&&ring.underrun==underrun);
        assert(port_pcm_available(&ring)==written-read&&written-read<=PORT_PCM_CAPACITY);
    }
    /* A full queue refuses new data; silence does not advance past future PCM. */
    port_pcm_reset(&ring);
    for(unsigned i=0;i<PORT_PCM_CAPACITY/1024;++i)assert(port_pcm_push(&ring,input,1024)==1024);
    assert(port_pcm_push(&ring,input,1)==0);
    for(unsigned i=0;i<PORT_PCM_CAPACITY/1024;++i)assert(port_pcm_pull(&ring,output,1024)==1024);
    assert(port_pcm_pull(&ring,output,17)==0);
    input[0]=-32768;input[1]=32767;
    assert(port_pcm_push(&ring,input,1)==1&&port_pcm_pull(&ring,output,1)==1);
    assert(output[0]==-32768&&output[1]==32767);
    puts("PCM queue: stereo/order/full/starvation/32-bit wrap PASS");return 0;
}
