#include <stdio.h> /* stderr, fprintf, ... */
#include <stdlib.h> /* malloc/free, ...  */
#include <fcntl.h> /* open/close, ... */
#include <errno.h> /*errno, strerror, ... */
#include <sys/mman.h> /* mmap MAP_XXX, PROT_XXX, ... */
#include <string.h> /* memset, memcpy, ... */
#include <signal.h> /* sigemptyset, sigaction, ... */
#include <asm/types.h> /* __u8, __u16, ... */

/* FIXME: This include is copied from linux-sunxi kernel!!! */
/* FIXME: This must be include after <asm/types.h> because sunxi_disp_ioctl.h uses these types but does not include the define!!! */
#include "sunxi_disp_ioctl.h" /* disp ioctls */
#include <linux/fb.h>

#define DISP_DEV "/dev/disp"
#define FB_DEV "/dev/fb0"

/* Maximum number of sprites in the sprite layer */
#define MAX_SPRITES 32

#define REQUIRED_VIRTUAL_FB 2

/* FIXME: This is used all over the place and it assumes we only have one screen configured */
#define SUNXIGFX_SCREEN 0

/* width must be multiple of 2, up to 512, height must be multiple of 2, up to 1024 */
#define SUNXIGFX_DEFAULT_SPRITE_WIDTH  512
#define SUNXIGFX_DEFAULT_SPRITE_HEIGHT 1024

typedef struct sunxiGFX_ctx {
  int dispFd; /* DISP_DEV file descriptor */
  int fbFd; /* FB_DEV file descriptor */
  __u32 screenWidth; /* pixels */
  __u32 screenHeight; /* pixels */
  __u32 fbBpp; /* framebuffer bits per pixel */
  __u32 fbLayerSize; /* size of actual framebuffer */
  __u32 fbVirtSize; /* size of virtual framebuffer */
  /* sprite layer offscreen framebuffer */
  __u32 fbPhysAddr; /* Address of the offscreen framebuffer mem (physical address) */
  __u8 *fbVirtAddr; /* Address of the offscreen framebuffer mem (virtual address) */
  /* temporal image blits offscreen framebuffer */
  __u32 fbPhysAddr2; /* Address of the 2nd offscreen framebuffer mem (physical address) */
  __u8 *fbVirtAddr2; /* Address of the 2nd offscreen framebuffer mem (virtual address) */
} sunxiGFX_ctx;

struct sunxiGFX_sprite {
  __u32 id;
  __u32 xOffset; /* x position of the start of this sprite in the mapped framebuffer (pixels) */
  __u32 yOffset; /* y position of the start of this sprite in the mapped framebuffer (pixels) */
};

struct sunxiGFX_sprite_layer {
  __u32 rows; /* number of rows of the sprite grid */
  __u32 cols; /* number of columns of the sprite grid */
  __u32 spriteWidth; /* width of each sprite (pixels) */
  __u32 spriteHeight; /* height of each sprite (pixels) */
  struct sunxiGFX_sprite *sprites; /* sprite grid */
  __u32 fbPhysAddr; /* Address of the sprites offscreen framebuffer mem (physical address) */
};

typedef enum {
  SUNXIGFX_OK = 0,
  SUNXIGFX_ERROR = 1
} sunxiGFX_status;

typedef struct sunxiGFX_sprite_layer sunxiGFX_sprite_layer;

static sunxiGFX_ctx *ctx;

/*** Internal functions ***/

static sunxiGFX_status sunxiGFXUninit();

static void sunxiGFXSignalHandler(int signal) {

  fprintf(stderr, "Executing signal handler, signal %d\n", signal);
  sunxiGFXUninit();
  exit(0);

}

static sunxiGFX_status sunxiGFXInstallSignalHandler(void(*handler) (int)) {

  struct sigaction act;

  act.sa_handler = handler;
  sigemptyset(&act.sa_mask);
  act.sa_flags = 0;

  if (sigaction(SIGINT, &act, NULL) < 0) {
    return SUNXIGFX_ERROR;
  }
  if (sigaction(SIGTERM, &act, NULL) < 0) {
    return SUNXIGFX_ERROR;
  }

  return SUNXIGFX_OK;
}

/* Initialize framebuffer info in context */
static sunxiGFX_status sunxiGFXInitFB() {

    struct fb_var_screeninfo fb_var;
    struct fb_fix_screeninfo fb_fix;
    int ret;

    ret = ioctl(ctx->fbFd, FBIOGET_FSCREENINFO, &fb_fix);
    if (ret < 0) {
      fprintf(stderr, "ERROR getting FB fixed info: %s\n",
	      strerror(-ret));
      return SUNXIGFX_ERROR;
    }

    ret = ioctl(ctx->fbFd, FBIOGET_VSCREENINFO, &fb_var);
    if (ret < 0) {
      fprintf(stderr, "ERROR getting FB variable info: %s\n",
	      strerror(-ret));
      return SUNXIGFX_ERROR;
    }

    ctx->screenWidth = fb_var.xres;
    ctx->screenHeight = fb_var.yres;
    ctx->fbBpp = fb_var.bits_per_pixel;
    ctx->fbLayerSize = fb_var.xres * fb_var.yres * (fb_var.bits_per_pixel >> 3);
    ctx->fbVirtSize = fb_fix.smem_len; /* or fb_var.xres_virtual * fb_var.yres_virtual * (fb_var.bits_per_pixel >> 3) */
    ctx->fbPhysAddr = fb_fix.smem_start;

    if (ctx->fbVirtSize < ctx->fbLayerSize*REQUIRED_VIRTUAL_FB) {
      fprintf(stderr, "Graphics layer size*%d > framebuffer size\n", REQUIRED_VIRTUAL_FB);
      return SUNXIGFX_ERROR;
    }

    /* mmap framebuffer memory */
    ctx->fbVirtAddr = (__u8 *) mmap(0, ctx->fbVirtSize,
				    PROT_READ | PROT_WRITE,
				    MAP_SHARED, ctx->fbFd, 0);
    if (ctx->fbVirtAddr == MAP_FAILED) {
      fprintf(stderr, "ERROR in mmap framebuffer memory: %s (errno=%d)\n",
	      strerror(errno), errno);
      return SUNXIGFX_ERROR;
    }

    ctx->fbVirtAddr2 = ctx->fbVirtAddr + ctx->fbLayerSize;
    ctx->fbPhysAddr2 = ctx->fbPhysAddr + ctx->fbLayerSize;

    return SUNXIGFX_OK;

}

static sunxiGFX_status sunxiGFXReleaseSprite(int disp, int aScreen, int aSprite) {

  __u32 args[4] = {aScreen, aSprite, 0, 0};

  /* DIS_OBJ_NOT_INITED (-4) is not an error, it means the sprite is not being used */
  if (ioctl(disp, DISP_CMD_SPRITE_BLOCK_RELEASE, args) != 0) {
    fprintf(stderr, "ERROR releasing sprite %d\n", aSprite);
    return SUNXIGFX_ERROR;
  }

  return SUNXIGFX_OK;

}

static sunxiGFX_status sunxiGFXReleaseAllSprites(int disp, int aScreen) {

  int i;
  /* Ignore success status */
  for (i = 0x64; i<=0x83; i++) sunxiGFXReleaseSprite(disp, aScreen, i);

}

/* Allocate sprite block and fill struct sunxiGFX_sprite */
/* XXX: width must be multiple of 2, up to 512, height must be multiple of 2, up to 1024 */
static sunxiGFX_status sunxiGFXAllocSprite(__u32 xoffset, __u32 yoffset, __u32 width, __u32 height, struct sunxiGFX_sprite *sprite) {

  __u32 args[4] = {SUNXIGFX_SCREEN, 0, 0, 0 };
  __disp_sprite_block_para_t sprite_block_para_in;

  memset(&sprite_block_para_in, 0, sizeof(__disp_sprite_block_para_t));
  /* use offscreen fb */
  sprite_block_para_in.fb.addr[0] = (unsigned long) ctx->fbPhysAddr;
  sprite_block_para_in.fb.size.width = ctx->screenWidth;
  sprite_block_para_in.fb.size.height = ctx->screenHeight;
  sprite_block_para_in.fb.format = DISP_FORMAT_ARGB888;
  sprite_block_para_in.fb.seq = DISP_SEQ_ARGB;
  sprite_block_para_in.fb.mode = DISP_MOD_INTERLEAVED;
  sprite_block_para_in.fb.br_swap = 0;
  sprite_block_para_in.fb.cs_mode = DISP_BT601;
  sprite_block_para_in.fb.b_trd_src = 0;
  /* source region,only care x,y because of not scaler */
  sprite_block_para_in.src_win.x = xoffset;
  sprite_block_para_in.src_win.y = yoffset;
  sprite_block_para_in.scn_win.x = xoffset;
  sprite_block_para_in.scn_win.y = yoffset;
  sprite_block_para_in.scn_win.width = width;
  sprite_block_para_in.scn_win.height = height;

  sprite->xOffset = xoffset;
  sprite->yOffset = yoffset;

  args[1] = (unsigned long) (&sprite_block_para_in);
  if ((sprite->id = ioctl(ctx->dispFd, DISP_CMD_SPRITE_BLOCK_REQUEST, args)) <= 0) {
    fprintf(stderr, "ERROR requesting a sprite block: %s\n", strerror(-sprite->id));
    return SUNXIGFX_ERROR;
  }

  return SUNXIGFX_OK;

}

/* FIXME: Clear what has already been done in case of error */
static sunxiGFX_status sunxiGFXInit() {

  int tmp = SUNXI_DISP_VERSION;

  ctx = (sunxiGFX_ctx *) malloc(sizeof(sunxiGFX_ctx));

  /* open DISP_DEV and do version handshake */
  ctx->dispFd = open(DISP_DEV, O_RDWR);
  if (ctx->dispFd == -1) {
    fprintf(stderr, "ERROR opening %s: %s\n", DISP_DEV, strerror(errno));
    return SUNXIGFX_ERROR;
  }
  if (ioctl(ctx->dispFd, DISP_CMD_VERSION, &tmp) < 0) {
    fprintf(stderr, "ERROR in disp handshake: %s\n", strerror(errno));
    return SUNXIGFX_ERROR;
  }

  /* open FB_DEV */
  ctx->fbFd = open(FB_DEV, O_RDWR);
  if (ctx->fbFd == -1) {
    fprintf(stderr, "ERROR opening %s: %s\n", FB_DEV, strerror(errno));
    return SUNXIGFX_ERROR;
  }

  /* Get framebuffer info (resolution, virtual resolution, mem address, ...) */
  sunxiGFXInitFB();

  if (sunxiGFXInstallSignalHandler(sunxiGFXSignalHandler) != SUNXIGFX_OK) {
    fprintf(stderr, "ERROR installing signal handler!!!\n");
  }

  return SUNXIGFX_OK;

}

/* It deinitializes all the library internals */
static sunxiGFX_status sunxiGFXUninit() {

  munmap(ctx->fbVirtAddr, ctx->fbVirtSize);

  close(ctx->fbFd);
  close(ctx->dispFd);

  free(ctx);

  sunxiGFXInstallSignalHandler(SIG_DFL);

  return SUNXIGFX_OK;

}

/* Allocate sprite layer, sprite width must be multiple of 2, up to 512,
 * sprite height must be multiple of 2, up to 1024
 */
/* FIXME: Clear what has already been done in case of error */
static sunxiGFX_status sunxiGFXAllocSpriteLayerInternal(__u32 spriteWidth,
							__u32 spriteHeight,
							sunxiGFX_sprite_layer **spriteLayer) {

  *spriteLayer = (sunxiGFX_sprite_layer *) malloc(sizeof(sunxiGFX_sprite_layer));
  (*spriteLayer)->spriteWidth = spriteWidth;
  (*spriteLayer)->spriteHeight = spriteHeight;
  (*spriteLayer)->fbPhysAddr = ctx->fbPhysAddr;

  __u32 args[4] = {SUNXIGFX_SCREEN, DISP_FORMAT_ARGB8888, DISP_SEQ_ARGB, 0};
  int ret;
  if ((ret = ioctl(ctx->dispFd, DISP_CMD_SPRITE_SET_FORMAT, args)) != 0) {
    fprintf(stderr, "ERROR setting sprite format: %s\n", strerror(-ret));
    return SUNXIGFX_ERROR;
  }

  /* map offscreen2 to sprites */
   (*spriteLayer)->rows = (ctx->screenHeight % spriteHeight == 0)?(ctx->screenHeight / spriteHeight):((ctx->screenHeight / spriteHeight) + 1);
   (*spriteLayer)->cols = (ctx->screenWidth % spriteWidth == 0)?(ctx->screenWidth / spriteWidth):((ctx->screenWidth / spriteWidth) + 1);
   if ((*spriteLayer)->rows * (*spriteLayer)->cols > MAX_SPRITES) {
     fprintf(stderr, "ERROR not enough %ux%u sprites available (%d) to map %ux%u screen\n", spriteWidth, spriteHeight, MAX_SPRITES, ctx->screenWidth, ctx->screenHeight);
     return SUNXIGFX_ERROR;
   }
  (*spriteLayer)->sprites = malloc((*spriteLayer)->rows * (*spriteLayer)->cols * sizeof(struct sunxiGFX_sprite));
  memset((*spriteLayer)->sprites, 0, (*spriteLayer)->rows * (*spriteLayer)->cols * sizeof(struct sunxiGFX_sprite));

  __u32 row, col;
  for (row = 0; row < (*spriteLayer)->rows; row++) {
    for (col = 0; col < (*spriteLayer)->cols; col++) {
      sunxiGFXAllocSprite(col * spriteWidth, row * spriteHeight, spriteWidth, spriteHeight, &((*spriteLayer)->sprites[row * (*spriteLayer)->cols + col]));
      fprintf(stderr, "Allocated sprite block %u (index %u) at pixel (%u, %u)\n",
	      (*spriteLayer)->sprites[row * (*spriteLayer)->cols + col].id,
	      row * (*spriteLayer)->cols + col,
	      (*spriteLayer)->sprites[row * (*spriteLayer)->cols + col].xOffset,
	      (*spriteLayer)->sprites[row * (*spriteLayer)->cols + col].yOffset);
    }
  }

  return SUNXIGFX_OK;

}

/* Allocate sprite layer */
static sunxiGFX_status sunxiGFXAllocSpriteLayer(sunxiGFX_sprite_layer **spriteLayer) {

  return sunxiGFXAllocSpriteLayerInternal(SUNXIGFX_DEFAULT_SPRITE_WIDTH,
					  SUNXIGFX_DEFAULT_SPRITE_HEIGHT,
					  spriteLayer);

}

/* Release sprite layer */
static sunxiGFX_status sunxiGFXFreeSpriteLayer(sunxiGFX_sprite_layer *spriteLayer) {

  int ret, i;
  __u32 args[4] = {SUNXIGFX_SCREEN, 0, 0, 0 };

  for (i=0; i < spriteLayer->rows * spriteLayer->cols; i++) {
    fprintf(stderr, "Releasing sprite block %u (index %u)\n", spriteLayer->sprites[i].id, i);
    args[1] = spriteLayer->sprites[i].id;
    if ((ret = ioctl(ctx->dispFd, DISP_CMD_SPRITE_BLOCK_RELEASE, args)) != 0) {
      fprintf(stderr, "ERROR releasing sprite block: %s\n", strerror(-ret));
    }
  }

  free(spriteLayer->sprites);
  free(spriteLayer);

  return SUNXIGFX_OK;

}

/* Make sprite layer visible */
/* TODO: Also, move all our sprites to top just in case there is some other application that uses sprites some day ... */
static sunxiGFX_status sunxiGFXShowSpriteLayer(sunxiGFX_sprite_layer *spriteLayer) {

  int ret;
  __u32 args[4] = {SUNXIGFX_SCREEN, 0, 0, 0};
  if ((ret = ioctl(ctx->dispFd, DISP_CMD_SPRITE_OPEN, args)) != 0) {
    fprintf(stderr, "ERROR opening sprite: %s\n", strerror(-ret));
    return SUNXIGFX_ERROR;
  }

  /* TODO: Needed??? */
  /* __u32 i; */
  /* for (i=0; i < spriteLayer->rows * spriteLayer->cols; i++) { */
  /*   fprintf(stderr, "Opening sprite block %u (index %u)\n", spriteLayer->sprites[i].id, i); */
  /*   args[1] = spriteLayer->sprites[i].id; */
  /*   if ((ret = ioctl(ctx->dispFd, DISP_CMD_SPRITE_BLOCK_OPEN, args)) != 0) { */
  /*     fprintf(stderr, "ERROR opening sprite block: %s\n", strerror(-ret)); */
  /*     return SUNXIGFX_ERROR; */
  /*   } */
  /* } */

  return SUNXIGFX_OK;

}

/* Make sprite layer not visible */
static sunxiGFX_status sunxiGFXHideSpriteLayer(sunxiGFX_sprite_layer *spriteLayer) {

  int ret;
  __u32 args[4] = {SUNXIGFX_SCREEN, 0, 0, 0};
  if ((ret = ioctl(ctx->dispFd, DISP_CMD_SPRITE_CLOSE, args)) != 0) {
    fprintf(stderr, "ERROR closing sprite: %s\n", strerror(-ret));
  }

  /* TODO: Needed??? */
  /* __u32 i; */
  /* for (i=0; i < spriteLayer->rows * spriteLayer->cols; i++) { */
  /*   fprintf(stderr, "Closing sprite block %u (index %u)\n", spriteLayer->sprites[i].id, i); */
  /*   args[1] = spriteLayer->sprites[i].id; */
  /*   if ((ret = ioctl(ctx->dispFd, DISP_CMD_SPRITE_BLOCK_CLOSE, args)) != 0) { */
  /*     fprintf(stderr, "ERROR closing sprite block: %s\n", strerror(-ret)); */
  /*     return SUNXIGFX_ERROR; */
  /*   } */
  /* } */

  return SUNXIGFX_OK;

}

int main(char argc, char **argv) {

  sunxiGFX_sprite_layer *spriteLayer;

  sunxiGFXInit();
  sunxiGFXAllocSpriteLayer(&spriteLayer);
  sunxiGFXShowSpriteLayer(spriteLayer);
  sleep(10);
  sunxiGFXHideSpriteLayer(spriteLayer);
  sunxiGFXFreeSpriteLayer(spriteLayer);
  sunxiGFXUninit();

}
