#ifndef PORT_PROFILE_H
#define PORT_PROFILE_H
enum { PORT_PROFILE_GPU,PORT_PROFILE_GTE,PORT_PROFILE_VM,PORT_PROFILE_SCALE,
       PORT_PROFILE_WAIT,PORT_PROFILE_CATEGORIES };
enum { PORT_PROFILE_TEXTURE, PORT_PROFILE_UPLOAD, PORT_PROFILE_PALETTE,
       PORT_PROFILE_MESH_PACKETS, PORT_PROFILE_OT_WALK, PORT_PROFILE_SPU, PORT_PROFILE_AUDIO_SINK,
       PORT_PROFILE_MOVIE_VLC, PORT_PROFILE_MOVIE_MDEC,
       PORT_PROFILE_SPU_VOICES, PORT_PROFILE_SPU_OUTPUT, PORT_PROFILE_SPU_STEADY, PORT_PROFILE_SPU_EVENT,
       PORT_PROFILE_DETAIL_CATEGORIES };
/* PORT_PROFILE: frame times and the categories' timers; with PORT_PROFILE_FRAME_ONLY (PROFILE=2) frame times
 * alone, so the total is what an ordinary build takes (the timers cost about 7% in a battle). */
#if defined(PORT_PROFILE) && !defined(PORT_PROFILE_FRAME_ONLY)
unsigned int port_profile_begin(void);
void port_profile_end(unsigned int category,unsigned int start);
void port_profile_draw_end(unsigned int kind,unsigned int start);
void port_profile_detail_end(unsigned int category,unsigned int start);
void port_profile_init(void);
void port_profile_frame(unsigned int mode,unsigned int frame);
#elif defined(PORT_PROFILE)
#define port_profile_begin() 0u
#define port_profile_end(category,start) ((void)(category),(void)(start))
#define port_profile_draw_end(kind,start) ((void)(kind),(void)(start))
#define port_profile_detail_end(category,start) ((void)(category),(void)(start))
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
