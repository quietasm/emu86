// emu86_vmm.c - 8086 Emulator simplified for HELLO.BIN test

#include <stdio.h>      //printf,scanf
#include <stdlib.h>     //malloc,free,exit
#include <conio.h>      //getch
#include <string.h>     //memset
#include <pthread.h>    //threads
#include <windows.h>    //WinAPI, unicode and localization
#include <tchar.h>
#include <locale.h>

/*------------------------------------*/
// types

typedef unsigned char byte;
typedef unsigned short word;
typedef unsigned int dword;

typedef struct {
   union {
      byte bregs[8];
      word wregs[8];
      struct {
         byte al,ah,cl,ch,dl,dh,bl,bh;
      } h;
      struct {
         word ax,cx,dx,bx,sp,bp,si,di;
      } x;
   } gr;
   union {
      word sregs[4];
      struct {
         word es,cs,ss,ds;
      } x;
   } sr;
   word ip, flags;
} regs_t;

#define TF 0x0100

typedef struct {
   word intr;
   int (*foo)(regs_t*); // service procedure, return IO request status (pending/done)
} isr_t;
// pending status means repeat call after message processing
#define MAXISR 8

typedef struct {
   byte len;
   byte opsize;
   byte cmd;
   byte dst,src;
   byte reg;
   word val;
} instr_t;

#define opNONE 0
#define opREG  1
#define opVAL  2

#define cmERR  0
#define cmMOV  1
#define cmINT  2
#define cmRET  3

typedef struct stream_tag {
   dword loc;
} stream_t;

#define ERROR_CREATE_THREAD -11
#define ERROR_JOIN_THREAD	-12

typedef struct msg_tag {
   byte id;
   byte param;
} msg_t;

#define MSG_EXIT        1
#define MSG_RESUME      2
#define MSG_EXCEPTION   3
#define MSG_CHAR        4

typedef struct {
   byte id;
} exc_t;

#define EXC_DE    0
#define EXC_DB    1
#define EXC_NMI   2
#define EXC_BP    3  // int3 or int 3
#define EXC_OF    4  // into or int 4
#define EXC_BC    5  // bound
#define EXC_UD    6
#define EXC_NM    7  // no FPU
#define EXC_GP    13

typedef struct mailbox_tag {
   pthread_mutex_t mutex;
   pthread_cond_t cond;
   int size, count;
   msg_t *buf;
} mailbox_t;

/*------------------------------------*/
// display

typedef struct char_tag {
   byte cha;
   byte clr;
} char_t;

#define BLACK        0
#define BLUE         1
#define GREEN        2
#define CYAN         3
#define RED          4
#define MAGENTA      5
#define SALAD        6
#define LIGHTGRAY    7
#define DARKGRAY     8
#define LIGHTBLUE    9
#define LIGHTGREEN   10
#define LIGHTCYAN    11
#define LIGHTRED     12
#define LIGHTMAGENTA 13
#define YELLOW       14
#define WHITE        15

/*------------------------------------*/
// prototypes

// memory device functions
dword addr(word seg, word ofs);
void read_byte(byte *v, dword adr);
void read_word(word *v, dword adr);
void write_byte(byte *v, dword adr);
void write_word(word *v, dword adr);

// stack functions
void push(word *v);
void pop(word *v);

byte getbyte(stream_t *s);
word getword(stream_t *s);
void load_arg(instr_t *ins, byte id, word *d);
void store_arg(instr_t *ins, byte id, word *s);

void decode(instr_t *ins);
void interpret(instr_t *ins);

void stop(char *msg);
void print_regs(void);
int intr20(regs_t *r);
int intr21(regs_t *r);

int mailbox_init(mailbox_t *q, int n);
int mailbox_destroy(mailbox_t *q);
void mailbox_send(mailbox_t *q, msg_t *m);
void mailbox_recv(mailbox_t *q, msg_t *m);
int mailbox_peek(mailbox_t *q, msg_t *m);

void* emulator(void* args);
void* debuger(void* args);

byte screen_attr(byte x);
void screen_clear();
void screen_scrollup();
void screen_putc(byte x);
void screen_puts(byte *s);

/*------------------------------------*/
// variables

#define MEMSIZE (1024*1024)
byte *memory;

#define filename "hello.bin"

#define LOADSEG 0x100
#define LOADIP 0x100
#define LOADSP 0x1000

regs_t regs;

byte exc_flag;
exc_t exc_msg;

isr_t isr[MAXISR];
word isrnum;

mailbox_t dbg, emu;

COLORREF palette[16] = {
   RGB(0x00,0x00,0x00), // BLACK
   RGB(0x00,0x00,0x8B), // BLUE
   RGB(0x00,0x64,0x00), // GREEN
   RGB(0x00,0xFF,0xFF), // CYAN
   RGB(0x8B,0x00,0x00), // RED
   RGB(0x8B,0x00,0x8B), // MAGENTA
   RGB(0xFF,0xD7,0x00), // SALAD (GOLD)
   RGB(0xD3,0xD3,0xD3), // LIGHTGRAY
   RGB(0xA9,0xA9,0xA9), // DARKGRAY
   RGB(0x00,0x00,0xFF), // LIGHTBLUE
   RGB(0x00,0x80,0x00), // LIGHTGREEN
   RGB(0xE0,0xFF,0xFF), // LIGHTCYAN
   RGB(0xFF,0x00,0x00), // LIGHTRED
   RGB(0xFF,0x00,0xFF), // LIGHTMAGENTA
   RGB(0xFF,0xFF,0x00), // YELLOW
   RGB(0xFF,0xFF,0xFF) // WHITE
};

char_t screen_buf[80*25];
char_t screen_char = { .cha=' ', .clr=WHITE*16+MAGENTA };
int screen_x, screen_y;

LRESULT CALLBACK WndProc(HWND,UINT,WPARAM,LPARAM);
TCHAR ClassName[] = "MainFrame";

/*------------------------------------*/

int APIENTRY WinMain(HINSTANCE hThis,
                     HINSTANCE hPrev,
                     LPSTR     lpCmdLine,
                     int       nCmdShow)
{
   FILE *fp;
   byte buf[80];
   int n, i;
   dword d;
   pthread_t emu_thread, dbg_thread;
   int status;
   int thread_stat;
   msg_t m;
   HWND hWnd;
   WNDCLASS wc;
   MSG msg;
	pthread_t dum;
	int dum_stat;

   setlocale(LC_CTYPE, "rus");

   /*--------------------------------*/

   wc.hInstance = hThis;
   wc.lpszClassName = ClassName;
   wc.lpfnWndProc = WndProc;
   wc.style = CS_HREDRAW | CS_VREDRAW;
   wc.hIcon = LoadIcon(0, IDI_APPLICATION);
   wc.hCursor = LoadCursor(0, IDC_ARROW);
   wc.lpszMenuName = 0;
   wc.cbClsExtra = 0;
   wc.cbWndExtra = 0;
   wc.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);

   if(!RegisterClass(&wc))
   {
      MessageBox(0, "Ошибка регистрации класса", "Ошибка", MB_ICONERROR|MB_OK);
      return 1;
   }

   screen_clear();

   /*--------------------------------*/

   memory = (byte*)malloc(MEMSIZE);
   if(!memory)
      stop("can't allocate memory");
   memset(memory,0,MEMSIZE);

   printf("memory initialized\n");

   /*--------------------------------*/

   fp = fopen(filename, "r");
   if(!fp)
      stop("can't open file");

   d = addr(LOADSEG,LOADIP);
   do {
      n = fread(buf,1,80,fp);
      i=0;
      while(i<n)
         memory[d++] = buf[i++];
   } while(n >= 80);
   fclose(fp);

   printf("image loaded\n");

   /*--------------------------------*/

   // initialize CPU context
   regs.ip = LOADIP;
   regs.sr.x.cs = LOADSEG;
   regs.gr.x.sp = LOADSP;
   regs.sr.x.ss = LOADSEG;
   regs.sr.x.es = regs.sr.x.ds = LOADSEG;
   regs.gr.x.ax = regs.gr.x.cx = regs.gr.x.dx =
      regs.gr.x.bx = regs.gr.x.si = regs.gr.x.di = 0;

   // write INT 20h to PSP
   *(word*)buf = 0x20CD;
   write_word((word*)buf, addr(LOADSEG,0));

   // push return address
   *(word*)buf = 0;
   push((word*)buf);

   printf("CPU & PSP initialized\n");

   /*--------------------------------*/

   isrnum = 0;
   isr[isrnum].intr = 0x20;
   isr[isrnum++].foo = intr20;
   isr[isrnum].intr = 0x21;
   isr[isrnum++].foo = intr21;

   /*--------------------------------*/

   //regs.flags |= TF;

   if(mailbox_init(&dbg, 3) != 0 || mailbox_init(&emu, 2) !=0)
      stop("main error: can't init mailbox\n");

	if(pthread_create(&dbg_thread, NULL, debuger, NULL) != 0 ||
         pthread_create(&emu_thread, NULL, emulator, NULL) != 0)
      stop("main error: can't create thread\n");

   /*--------------------------------*/

   hWnd = CreateWindow("MainFrame", "Каркас win32 приложения",
                        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                           400, 300, HWND_DESKTOP, 0, hThis, 0);
   if(hWnd == 0)
   {
      MessageBox(0, "Ошибка создания окна", "Ошибка", MB_ICONERROR|MB_OK);
      return 1;
   }
   ShowWindow(hWnd, nCmdShow);

   while(GetMessage(&msg, 0, 0, 0))
   {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
   }

   /*--------------------------------*/

   m.id = MSG_EXIT;
   mailbox_send(&emu, &m);
   mailbox_send(&dbg, &m);

   printf("main: waiting threads...\n");

	status = pthread_join(emu_thread, (void**)&thread_stat);
	if(status == 0) {
      printf("joined with address %d\n", thread_stat);
      status = pthread_join(dbg_thread, (void**)&thread_stat);
      if(status == 0)
         printf("joined with address %d\n", thread_stat);
   }
	if(status != 0)
		stop("main error: can't join thread\n");

	mailbox_destroy(&emu);
	mailbox_destroy(&dbg);

   return msg.wParam;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wp, LPARAM lp)
{
   PAINTSTRUCT ps;
   HDC hDC;
   TEXTMETRIC tmText;
   int dx, dy, i, j;
   RECT rc;
   HFONT hFont, hOldFont;
   char_t ch;
   COLORREF crOldColor, crOldBkColor;
   msg_t m;

   switch(uMsg)
   {
      case WM_CHAR:
         m.param = (byte)wp;
         m.id = MSG_CHAR;
         mailbox_send(&dbg, &m);
         break;

      case WM_PAINT:
         hDC = BeginPaint(hWnd, &ps);

         hFont = GetStockObject(OEM_FIXED_FONT);
         hOldFont = SelectObject(hDC, hFont);

         GetTextMetrics(hDC, &tmText);
         dx = tmText.tmMaxCharWidth;
         dy = tmText.tmHeight;

         crOldColor = SetTextColor(hDC, palette[LIGHTGRAY]);
         crOldBkColor = SetBkColor(hDC, palette[BLACK]);

         rc.top = 0;
         rc.bottom = dy;
         for(i=0; i<25; i++) {
            rc.left = 0;
            rc.right = dx;
            for(j=0; j<80; j++) {
               ch = screen_buf[i*80+j];
               SetTextColor(hDC, palette[ch.clr & 15]);
               SetBkColor(hDC, palette[ch.clr >> 4]);
               TextOut(hDC, rc.left, rc.top, &ch.cha, 1);
               rc.left += dx;
               rc.right += dx;
            }
            rc.top += dy;
            rc.bottom += dy;
         }
         // draw cursor - small rectangle over the bottom
         // of char in the specified position

         SelectObject(hDC, hOldFont);
         SetTextColor(hDC, crOldColor);
         SetBkColor(hDC, crOldBkColor);
         EndPaint(hWnd, &ps);
         break;

      case WM_DESTROY:
         PostQuitMessage(0);
         break;

      default:
         return DefWindowProc(hWnd,uMsg,wp,lp);
   }
   return 0;
}

/*------------------------------------*/

void* emulator(void* args) {
   msg_t m;
   instr_t ins;
   int running = 1, flag;
   exc_flag = 0;
   while(1) {
      if(running) // to prevent active waiting
         flag = mailbox_peek(&emu, &m);
      else {
         flag = 1;
         mailbox_recv(&emu, &m);
      }
      if(flag) {
         switch(m.id) {
            case MSG_EXIT:
               goto end;
            case MSG_RESUME:
               running = 1;
               break;
         }
      }
      if(running) {
         //if(inta) { exc_t e = { .id = irqn }; inta=0; exception(&e); }
         decode(&ins);
         interpret(&ins);
         if(exc_flag == 0 && (regs.flags & TF)) {
            exc_flag = 1;
            exc_msg.id = EXC_DB;
         }
         if(exc_flag) {
            exc_flag = 0;
            m.id = MSG_EXCEPTION;
            m.param = exc_msg.id;
            mailbox_send(&dbg, &m);
            running = 0;
         }
      }
   }
end:
   printf("emulator stopped\n");
   return NULL;
}

/*------------------------------------*/

#define PS_RUNNING   1
#define PS_PENDING   2
#define PS_DONE      3

int app_state;
int ch_max = 10, ch_ptr;
byte ch_buf[10];
int (*sysproc)(regs_t*);

void* debuger(void* args) {
   msg_t m;
   instr_t ins;
   int i;
   char* errmsg = NULL;
   app_state = PS_RUNNING;
   int running = 1;

   ch_ptr = 0;
   while(running) {
      mailbox_recv(&dbg, &m);
      if(1) {
         switch(m.id) {
            case MSG_EXCEPTION:
               printf("exception at %X:%X\n", regs.sr.x.cs, regs.ip);
               switch(m.param) {
                  case EXC_DB:
                     printf("#DB\n", regs.sr.x.cs, regs.ip);
                     getch();
                     break;
                  case EXC_UD:
                     errmsg = "unknown instruction";
                     app_state = PS_DONE;
                     break;
                  case EXC_GP:
                     decode(&ins);
                     switch(ins.cmd) {
                        case cmINT:
                           i = 0;
                           while(i < isrnum && isr[i].intr != ins.val)
                              i++;
                           if(i < isrnum) {
                              sysproc = isr[i].foo;
                              app_state = PS_PENDING; //(*isr[i].foo)(&regs)
                           } else {
                              errmsg = "unhandled interrupt";
                              app_state = PS_DONE;
                           }
                           regs.ip += ins.len;
                           break;

                        default:
                           errmsg = "unexpected error";
                           app_state = PS_DONE;
                     }
                     break;
                  default:
                     errmsg = "unhandled exception";
                     app_state = PS_DONE;
               }
               if(app_state == PS_RUNNING) {
                  m.id = MSG_RESUME;
                  mailbox_send(&emu, &m);
               } else if(app_state == PS_DONE) {
                  printf("program terminated\n");
               }
               break;

            case MSG_EXIT:
               running = 0;
               app_state = PS_DONE;
               printf("stopping debuger\n");
               break;

            case MSG_CHAR:
               if(ch_ptr < ch_max) {
                  ch_buf[ch_ptr++] = m.param;
                  printf("'%c' added to buf\n", m.param);
               }
               break;
         }
      }
      if(app_state == PS_PENDING) {
         app_state = (*sysproc)(&regs);
         if(app_state == PS_RUNNING) {
            m.id = MSG_RESUME;
            mailbox_send(&emu, &m);
         } else if(app_state == PS_DONE) {
            printf("program terminated\n");
         }
      }
   }
   printf("debuger stopped\n");

   if(errmsg != NULL) {
      printf("error: %s\n", errmsg);
   }

   return NULL;
}

// return process state (PS_RUNNING by default)
int intr20(regs_t *r)
{
   return PS_DONE;
}

int intr21(regs_t *r)
{
   switch((*r).gr.h.ah)
   {
      case 8: {
         if(ch_ptr == 0) {
            return PS_PENDING;
         }
         //int t = getch();
         (*r).gr.h.al = ch_buf[--ch_ptr]; //(byte)(t & 0xFF);
         break;
      }
      case 9: {
         dword p = addr((*r).sr.x.ds, (*r).gr.x.dx);
         char buf[80];

         int i = 0;
         while(i < 80 && memory[p] != '$')
            buf[i++] = (char)memory[p++];

         if(i < 80)
            buf[i] = '\0';
         else
            stop("ascii$ too long"); // !!!
         //printf("%s",buf);
         screen_puts(buf);
         break;
      }
      default:
         stop("unknown DOS function"); // !!!
   }
   (*r).flags &= ~1;
   return PS_RUNNING;
}

/*------------------------------------*/

void screen_clear() {
   int i;
   for(i=0; i<80*25; i++)
      screen_buf[i] = screen_char;
   screen_x = 0;
   screen_y = 0;
}

void screen_scrollup() {
   int i;
   for(i=0; i<80*24; i++)
      screen_buf[i] = screen_buf[i+80];
   for(; i<80*25; i++)
      screen_buf[i] = screen_char;
}

void screen_putc(byte x) {
   if(x == 10) {
      screen_x = 0;
      screen_y++;
      if(screen_y >= 25) {
         screen_scrollup();
         screen_y = 24;
      }
   } else {
      screen_buf[screen_y*80+screen_x].cha = x;
      screen_buf[screen_y*80+screen_x].clr = screen_char.clr;
      screen_x++;
      if(screen_x >= 80) {
         screen_x = 0;
         screen_y++;
         if(screen_y >= 25) {
            screen_scrollup();
            screen_y = 24;
         }
      }
   }
}

void screen_puts(byte *s) {
   while(*s != '\0') screen_putc(*s++);
}

byte screen_attr(byte x) {
   byte old = screen_char.clr;
   screen_char.clr = x;
   return old;
}

/*------------------------------------*/

void stop(char *msg)
{
   printf("%s\n", msg);

   print_regs();

   if(memory) {
      char ch;

      printf("Create memory dump? [Y/N] ");
      scanf("%c",&ch);

      if(ch == 'Y' || ch == 'y') {
         FILE *fp = fopen("memory","w+");
         if(fp) {
            fwrite(memory,1,1024*1024,fp);
            fclose(fp);
         }
      }
      free(memory);
   }

   //printf("Press any key...\n");
   //getch();

   exit(0);
}

void print_regs()
{
   printf("[registers]\n");
   printf("AX=%04X\tBX=%04X\tCX=%04X\tDX=%04X\n", regs.gr.x.ax,
                           regs.gr.x.bx, regs.gr.x.cx, regs.gr.x.dx);
   printf("SI=%04X\tDI=%04X\tBP=%04X\tSP=%04X\n", regs.gr.x.si,
                           regs.gr.x.di, regs.gr.x.bp, regs.gr.x.sp);
   printf("ES=%04X\tCS=%04X\tSS=%04X\tDS=%04X\n", regs.sr.x.es,
                           regs.sr.x.cs, regs.sr.x.ss, regs.sr.x.ds);
   printf("IP=%04X\tFLAGS=%04X\n", regs.ip, regs.flags);
}

/*------------------------------------*/

dword addr(word seg, word ofs)
{
   return seg*16+ofs;
}

void read_byte(byte *v, dword adr)
{
   *v = *(memory+adr);
}

void read_word(word *v, dword adr)
{
   *v = *(word*)(memory+adr);
}

void write_byte(byte *v, dword adr)
{
   *(memory+adr) = *v;
}

void write_word(word *v, dword adr)
{
   *(word*)(memory+adr) = *v;
}

/*------------------------------------*/

void push(word *v)
{
   regs.gr.x.sp -= 2;
   write_word(v, addr(regs.sr.x.ss, regs.gr.x.sp));
}

void pop(word *v)
{
   read_word(v, addr(regs.sr.x.ss, regs.gr.x.sp));
   regs.gr.x.sp += 2;
}

/*------------------------------------*/

byte getbyte(stream_t *s)
{
   byte x;
   read_byte(&x, (*s).loc);
   (*s).loc += 1;
   return x;
}

word getword(stream_t *s)
{
   word x;
   read_word(&x, (*s).loc);
   (*s).loc += 2;
   return x;
}

/*------------------------------------*/

void decode(instr_t *ins)
{
   stream_t s;
   dword org;
   byte opc;

   s.loc = addr(regs.sr.x.cs, regs.ip);
   org = s.loc;

   (*ins).opsize = 2;
   (*ins).dst = (*ins).src = opNONE;

   opc = getbyte(&s);

   if((opc & 0xF0) == 0xB0)
   {
      (*ins).cmd = cmMOV;
      if((opc & 8) == 0)
         (*ins).opsize = 1;
      (*ins).dst = opREG;
      (*ins).reg = opc & 7;
      (*ins).src = opVAL;
      if((*ins).opsize == 1)
         (*ins).val = getbyte(&s);
      else
         (*ins).val = getword(&s);
   }
   else if(opc == 0xCD)
   {
      (*ins).cmd = cmINT;
      (*ins).dst = opVAL;
      (*ins).val = getbyte(&s);
   }
   else if(opc == 0xC3)
      (*ins).cmd = cmRET;
   else {
      (*ins).len = 0;
      (*ins).cmd = cmERR;
      return;
   }

   (*ins).len = (byte)(s.loc-org);
}

// выполняет команду, изменяет регистры и содержимое
// памяти, в т.ч. регистры CS:IP
void interpret(instr_t *ins)
{
   int i, setpc = 1;
   word x;
   exc_t e;

   if((*ins).len <= 0) {
      exc_flag = 1;
      exc_msg.id = EXC_UD;
      return;
   }

   switch((*ins).cmd)
   {
      case cmMOV:
         load_arg(ins, (*ins).src, &x);
         store_arg(ins, (*ins).dst, &x);
         break;

      case cmRET:
         pop(&regs.ip);
         return; // ~ setpc = 0; break;

      case cmINT:
         exc_flag = 1;
         exc_msg.id = EXC_GP;
         return; // ~

      default:
         exc_flag = 1;
         exc_msg.id = EXC_UD;
         return; // ~
   }

   if(setpc)
      regs.ip += (*ins).len;
}

/*------------------------------------*/

void load_arg(instr_t *ins, byte id, word *d)
{
   word t;
   switch(id)
   {
      case opREG:
         if((*ins).opsize == 1) {
            // AL CL DL BL AH CH DH BH
            // 0  1  2  3  4  5  6  7
            // 0  2  4  6  1  3  5  7
            t = (*ins).reg;
            if(t < 4)
               t = t*2;
            else
               t = (t-4)*2+1;
            *d = regs.gr.bregs[t];
         } else
            // 'reg' is index of register
            *d = regs.gr.wregs[(*ins).reg];
         break;

      case opVAL:
         *d = (*ins).val;
         break;

      default:
         stop("bad argument");
   }
}

void store_arg(instr_t *ins, byte id, word *s)
{
   word t;
   switch(id)
   {
      case opREG:
         if((*ins).opsize == 1) {
            t = (*ins).reg;
            if(t < 4)
               t = t*2;
            else
               t = (t-4)*2+1;
            regs.gr.bregs[t] = *s;
         } else
            regs.gr.wregs[(*ins).reg] = *s;
         break;

      default:
         stop("bad argument");
   }
}

/*------------------------------------*/

int mailbox_init(mailbox_t *q, int n) {
   pthread_mutex_init(&q->mutex, NULL);
   pthread_cond_init(&q->cond, NULL);
   q->buf = (msg_t*)malloc(n*sizeof(msg_t));
   q->count = 0;
   q->size = n;
   return 0;
}

int mailbox_destroy(mailbox_t *q) {
   free(q->buf);
   pthread_mutex_destroy(&q->mutex);
   pthread_cond_destroy(&q->cond);
   return 0;
}

void mailbox_send(mailbox_t *q, msg_t *m) {
   pthread_mutex_lock(&q->mutex);
   while(q->count >= q->size) // ждём, пока нет требуемого состояния
      pthread_cond_wait(&q->cond, &q->mutex); // empty
   q->buf[q->count++] = *m;
   pthread_mutex_unlock(&q->mutex);
   pthread_cond_signal(&q->cond); // full
}

void mailbox_recv(mailbox_t *q, msg_t *m) {
   int i;
   pthread_mutex_lock(&q->mutex);
   while(q->count == 0)
      pthread_cond_wait(&q->cond, &q->mutex); // full
   *m = q->buf[0];
   q->count--;
   for(i=0; i<q->count; i++)
      q->buf[i] = q->buf[i+1];
   pthread_mutex_unlock(&q->mutex);
   pthread_cond_signal(&q->cond); // empty
}

int mailbox_peek(mailbox_t *q, msg_t *m) {
   int flag = 0, i;
   pthread_mutex_lock(&q->mutex);
   if(q->count != 0) {
      *m = q->buf[0];
      q->count--;
      for(i=0; i<q->count; i++)
         q->buf[i] = q->buf[i+1];
      flag = 1;
   }
   pthread_mutex_unlock(&q->mutex);
   return flag;
}
