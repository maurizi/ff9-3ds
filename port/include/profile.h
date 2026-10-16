#ifndef PORT_PROFILE_H
#define PORT_PROFILE_H
enum { PORT_PROFILE_GPU,PORT_PROFILE_GTE,PORT_PROFILE_VM,PORT_PROFILE_SCALE,
       PORT_PROFILE_WAIT,PORT_PROFILE_CATEGORIES };
enum { PORT_PROFILE_TEXTURE, PORT_PROFILE_UPLOAD, PORT_PROFILE_PALETTE,
       PORT_PROFILE_MESH_PACKETS, PORT_PROFILE_OT_WALK, PORT_PROFILE_SPU, PORT_PROFILE_AUDIO_SINK, PORT_PROFILE_DETAIL_CATEGORIES };
#ifdef PORT_PROFILE
unsigned int port_profile_begin(void);
void port_profile_end(unsigned int category,unsigned int start);
void port_profile_draw_end(unsigned int kind,unsigned int start);
void port_profile_detail_end(unsigned int category,unsigned int start);
void port_profile_init(void);
void port_profile_frame(unsigned int mode,unsigned int frame);
#else
#define port_profile_begin() 0u
#define port_profile_end(category,start) ((void)(category),(void)(start))
#define port_profile_draw_end(kind,start) ((void)(kind),(void)(start))
#define port_profile_detail_end(category,start) ((void)(category),(void)(start))
#define port_profile_init() ((void)0)
#define port_profile_frame(mode,frame) ((void)(mode),(void)(frame))
#endif
#endif
