/* Journal driver for unmodified pinned DuckStation voice methods. The driver
 * supplies register events, sample scheduling and silent CD capture; it is not
 * an independent CPU bus/IRQ/DMA or complete SPU implementation. */
#include <cinttypes>
#include <cstdlib>
static void journal_error(const char* message) {
    std::fprintf(stderr,"Voice replay: %s\n",message);std::exit(2);
}
static u32 journal_u32(FILE* file) {
    u8 b[4];if(std::fread(b,1,4,file)!=4)journal_error("truncated journal");
    return u32(b[0])|(u32(b[1])<<8)|(u32(b[2])<<16)|(u32(b[3])<<24);
}
int main(int argc,char** argv) {
    if(argc!=2)journal_error("journal required");
    FILE* file=std::fopen(argv[1],"rb");if(!file)journal_error("cannot open journal");
    char magic[8];if(std::fread(magic,1,8,file)!=8||std::memcmp(magic,"FF9SPU1\0",8))journal_error("bad signature");
    u16 regs[512]{};u32 off=0,capture=0;
    u64 samples=0,mismatches[4]{},first[4];s32 first_actual[4]{},first_reference[4]{};
    s64 max_error[4]{};for(auto& value:first)value=UINT64_MAX;
    bool initialized=false;
    for(;;) {
        u32 type=journal_u32(file),size=journal_u32(file);
        if(type==UINT32_MAX) {
            if(size||std::fgetc(file)!=EOF)journal_error("invalid footer");break;
        }
        if(type==0&&!size) {
            SPU::s_state={};SPU::s_state.noise_level=1;SPU::noise=1;
            std::memset(SPU::ram,0,sizeof SPU::ram);std::memset(regs,0,sizeof regs);
            off=capture=0;initialized=true;continue;
        }
        if(!initialized)journal_error("missing reset");
        if(type==1&&size==8) {
            u32 address=journal_u32(file),value=journal_u32(file);
            if(address>=0x400||(address&1)||value>65535)journal_error("invalid register event");
            u16 old=regs[address/2];regs[address/2]=value;
            if(address<0x180)SPU::WriteVoiceRegister(address,value);
            else if(address==0x188||address==0x18a) {
                u32 shift=(address&2)*8;
                SPU::s_state.key_on_register=((SPU::s_state.key_on_register&~(0xffffu<<shift))|(value<<shift))&0xffffff;
            } else if(address==0x18c||address==0x18e) {
                u32 shift=(address&2)*8;off=((off&~(0xffffu<<shift))|(value<<shift))&0xffffff;
            } else if(address==0x1aa) {
                SPU::s_state.SPUCNT.noise_clock=(value>>8)&63;
                if((old&0x8000)&&!(value&0x8000))for(auto& voice:SPU::s_state.voices)voice.ForceOff();
            } else if(address>=0x200&&address<0x260) {
                auto& voice=SPU::s_state.voices[(address-0x200)/4];
                ((address&2)?voice.right_volume:voice.left_volume).current_level=s16(value);
            }
        } else if(type==2&&size>=8) {
            u32 address=journal_u32(file),count=journal_u32(file);
            if(count!=size-8||address>0x80000||count>0x80000-address)journal_error("invalid RAM event");
            if(std::fread(SPU::ram+address,1,count,file)!=count)journal_error("truncated RAM event");
        } else if(type==3&&size==32) {
            s32 actual[8];for(auto& value:actual)value=s32(journal_u32(file));
            s32 expected[4]{},previous=0;
            auto flags=[&](unsigned address){return u32(regs[address/2])|(u32(regs[address/2+1])<<16);};
            for(unsigned i=0;i<24;++i) {
                auto [l,r]=SPU::SampleVoice(SPU::s_state.voices[i],i,(flags(0x194)>>i)&1,
                    i&&((flags(0x190)>>i)&1),regs[0x1aa/2]&0x40,previous);
                previous=SPU::s_state.voices[i].last_volume;
                expected[0]+=l;expected[1]+=r;
                if((flags(0x198)>>i)&1){expected[2]+=l;expected[3]+=r;}
            }
            if(!(regs[0x1aa/2]&0x4000))for(auto& value:expected)value=0;
            expected[2]=Clamp16(expected[2]);expected[3]=Clamp16(expected[3]);
            SPU::UpdateNoise();SPU::noise=s16(SPU::s_state.noise_level);
            for(unsigned ch=0;ch<2;++ch) {
                SPU::ram[ch*0x400+capture]=SPU::ram[ch*0x400+capture+1]=0;
                s16 value=s16(Clamp16(SPU::s_state.voices[1+ch*2].last_volume));
                SPU::ram[(2+ch)*0x400+capture]=u16(value);SPU::ram[(2+ch)*0x400+capture+1]=u16(value)>>8;
            }
            capture=(capture+2)&0x3ff;
            for(unsigned i=0;i<4;++i) {
                s64 error=s64(actual[i])-expected[i];if(error<0)error=-error;
                if(error) {
                    ++mismatches[i];max_error[i]=std::max(max_error[i],error);
                    if(first[i]==UINT64_MAX){first[i]=samples;first_actual[i]=actual[i];first_reference[i]=expected[i];}
                }
            }
            for(unsigned i=0;i<24;++i) {
                if((off>>i)&1)SPU::s_state.voices[i].KeyOff();
                if((SPU::s_state.key_on_register>>i)&1){SPU::s_state.voices[i].KeyOn();SPU::s_state.endx_register&=~(1u<<i);}
            }
            off=SPU::s_state.key_on_register=0;++samples;
        } else journal_error("invalid event type/size");
    }
    std::fclose(file);
    if(!samples)journal_error("journal contains no samples");
    std::printf("{\"samples\":%" PRIu64 ",\"stages\":[",samples);
    for(unsigned i=0;i<4;++i) {
        if(i)std::putchar(',');
        std::printf("{\"mismatched_values\":%" PRIu64 ",\"max_absolute_error\":%" PRId64 ",\"first_sample\":",mismatches[i],max_error[i]);
        if(first[i]==UINT64_MAX)std::printf("null");else std::printf("%" PRIu64,first[i]);
        std::printf(",\"first_actual\":%d,\"first_reference\":%d}",first_actual[i],first_reference[i]);
    }
    std::puts("]}");return 0;
}
