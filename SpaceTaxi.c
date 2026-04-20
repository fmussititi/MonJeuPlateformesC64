/*=============================================================================
 *  Platformer C64 – oscar64
 *  Architecture : IRQ raster cadencé à la frame, logique séparée du rendu
 *============================================================================*/

#include <c64/joystick.h>
#include <c64/vic.h>
#include <c64/rasterirq.h>
#include <c64/sprites.h>
#include <c64/memmap.h>
#include <c64/kernalio.h>
#include <c64/flossiec.h>
#include <oscar.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

/*=============================================================================
 *  Constantes
 *============================================================================*/

#define PLAYER_SPRITE    0

/* Physique – arithmétique sub-pixel 8.8 (>>8 pour obtenir les pixels réels) */
#define PLAYER_ACC_X     300    /* accélération horizontale par frame         */
#define PLAYER_FRICTION  200    /* décélération quand la direction est relâchée*/
#define PLAYER_MAX_VX    900    /* vitesse horizontale max                     */
#define PLAYER_GRAVITY   100    /* gravité par frame                           */
#define PLAYER_JUMP_VY  (-1800) /* impulsion de saut (négatif = vers le haut)  */
#define PLAYER_HEIGHT 21
#define PLAYER_WIDTH  12   // approximatif, on affinera plus tard
/* Vitesse verticale max = 1 tile par frame pour éviter le tunnel */
#define PLAYER_MAX_VY   (8 << 8)   /* 8 pixels/frame en sub-pixel */

#define MAX_ENEMIES 4
#define ENEMIES_GRAVITY   100    /* gravité par frame */
#define ENEMIES_MAX_VY   (8 << 8) /* 8 pixels/frame en sub-pixel */ 
#define ENEMIES_HEIGHT 21
#define ENEMIES_WIDTH  12
#define AI_PATROL   0
#define AI_CHASE    1

#define MAP_W 40
#define MAP_H 23
#define MAP_Y_OFF   1    /* la map commence à la ligne 1 de l'écran */

#define TILE_EMPTY 0
#define TILE_SOLID 1

#define SCREEN_OFFSET_X  24   /* offset VIC horizontal */
#define SCREEN_OFFSET_Y  50   /* offset VIC vertical   */

#define WORLD_MIN_X  0
#define WORLD_MAX_X  (MAP_W * 8 - PLAYER_WIDTH)  /* 40*8-12 = 308 */

#define WORLD_MAX_Y  (MAP_H * 8 - PLAYER_HEIGHT)  /* sol à la tile 20 */

#define CHAR_EMPTY  32    /* espace */
#define CHAR_SOLID  160   /* bloc plein (caractère inversé) */

#define CHAR_HEART 83   // caractère PETSCII cœur (ou un autre)

#define MAX_LEVELS 3

/*=============================================================================
 *  Données Charpad
 *============================================================================*/

static const uint8_t Monde1Collision[5] = {0,0,0,1,0};
static const uint8_t Monde2Collision[5] = {0,0,0,1,0};
static const uint8_t Monde3Collision[5] = {0,0,0,1,0};

char * const mondeTiles   = (char *)0x5000;   // 0x5000 256 octets  → fin $5100
char * const mondeColor   = (char *)0x5100;   // 0x5100 1000 octets → fin $54E8
char * const mondeMap     = (char *)0x6000;   // 0x5500 1000 octets → fin $58E8
char * const mondeCharset = (char *)0x7000;   // 0x5900 2048 octets → fin $6100

// Variables globales
floss_blk levelBlks[12];  // 4 fichiers × 3 niveaux

char * const Charset = (char *)0x3800;  /* zone libre en RAM */

/*=============================================================================
 *  Données du sprite joueur  (24×21 pixels, multicolore échiqueté)
 *============================================================================*/

const uint8_t sprite_player[64] = {
    0b00000000,0b00000000,0b00000000,
    0b00000000,0b00000000,0b00000000,
    0b00000000,0b00000000,0b00000000,
    0b00000000,0b10101010,0b00000000,
    0b00001010,0b10101010,0b10101010,
    0b00000101,0b01111101,0b11110000,
    0b00011101,0b11111101,0b11111111,
    0b00011101,0b01111111,0b01111111,
    0b00010111,0b11111101,0b01010000,
    0b00001111,0b11111111,0b11000000,
    0b00000101,0b10010101,0b00000000,
    0b00010101,0b10010110,0b01010100,
    0b01010101,0b10101010,0b01010101,
    0b11110111,0b11111010,0b10011111,
    0b11111111,0b10101010,0b11111111,
    0b11111010,0b10101010,0b10101111,
    0b11111010,0b00000000,0b10101111,
    0b00001010,0b00000000,0b10101111,
    0b00001010,0b00000000,0b10100000,
    0b00010101,0b00000000,0b01010100,
    0b01010101,0b00000000,0b01010101,
    0x00
};

const uint8_t sprite_enemy[64] = {
    0b00000000,0b00000000,0b00000000,
    0b00000000,0b00000000,0b00000000,
    0b00000000,0b00000000,0b00000000,
    0b00000000,0b00000000,0b00000000,
    0b00000000,0b00000000,0b00000000,
    0b00000000,0b00111100,0b00000000,
    0b00000000,0b11111100,0b00000000,
    0b00000000,0b11111111,0b00000000,
    0b00000011,0b11111111,0b00000000,
    0b00000001,0b11111101,0b11000000,
    0b00001110,0b01110110,0b11000000,
    0b00001110,0b01010110,0b11000000,
    0b00111110,0b01110110,0b11110000,
    0b00111110,0b10111010,0b11110000,
    0b00111111,0b11111111,0b11110000,
    0b00001111,0b10101011,0b11000000,
    0b00000010,0b10101010,0b00000000,
    0b00000001,0b10101010,0b00000000,
    0b00000101,0b01101001,0b00000000,
    0b00000101,0b01101001,0b00000000,
    0b00000001,0b01100101,0b00000000,
    0x00
};


/*=============================================================================
 *  Données Musique
 *============================================================================*/

#pragma section( music, 0)

#pragma region( music, 0xa000, 0xc000, , , {music} )

#pragma data(music)

__export const char music[] = {
	#embed 0x2000 0x7e "./assets/Breakfast.sid" 
};

#pragma data(data)


/*=============================================================================
 *  Adresses mémoire
 *============================================================================*/

char * const Screen    = (char *)0x0400;  /* écran texte par défaut           */
char * const ColorRAM  = (char *)0xD800;
char * const SpriteRAM = (char *)0x0340;  /* données sprites (hors BASIC)     */

static uint8_t sprite_base; /* index de bloc sprite (addr / 64)              */

RIRQCode hud;
RIRQCode game;
RIRQCode bottom;
RIRQCode rirq_music;

/*=============================================================================
 *  État du joueur
 *============================================================================*/

static bool playerOnGround = false;

// Spawn cohérent avec la map
static int playerX  = 20;   // tile 2, soit X=16 en pixels monde
static int playerY  = 0;    // haut de l'écran visible
static long playerXf = 20L << 8;
static long playerYf = 0L  << 8;
static long playerVx = 0;
static long playerVy = 0;

static bool playerIsDead = false;
static int respawnTimer = 0;

static int playerHP = 3;
static int playerInvTimer = 0;   // frames d’invincibilite
static int playerScore = 0;
static uint8_t hudDirty;
static int viewX = 0;   // en tiles (0 à 80 si map=120)
static int worldMaxX;
static int currentPage = -1;  // forcer le dessin au premier frame

/*=============================================================================
 *  Structs
 *============================================================================*/
static bool  needLevelChange = false;

typedef struct {
    long x, y;
    long xf, yf;
    long vx, vy;      // ← vy ajouté
    bool active;
    uint8_t spriteId;

    uint8_t aiMode;   // 0 = patrouille, 1 = poursuite
    int page;
} Enemy;

typedef struct {
    struct {
        long x, y;
        int vx;
        int page;
        bool isAlreadySpawn;
    } enemies[MAX_ENEMIES];
    uint8_t enemyCount;

    /* Données CharPad */
    char* tiles;
    int tilesCount;
    char* color;
    char* map;
    int mapWidth;   // en tiles
    int mapHeight;  // en tiles
    char* chars;
    const uint8_t *tileCollision;
    uint8_t     color_back;   /* couleur de fond Bg0 */
    uint8_t     color_back1;  /* M1 */
    uint8_t     color_back2;  /* M2 */
    uint8_t     color_border;
} LevelData;

/*=============================================================================
 *  Données sur les niveaux du jeu
 *============================================================================*/

static int currentLevel = 0;

LevelData levels[MAX_LEVELS] = {
    /* Niveau 0 */
    {
        .enemies       = {{80, 0, 200, 0, false}, {650, 0, -200, 2, false}},
        .enemyCount    = 2,
        .tileCollision = Monde1Collision,
        .tiles         = mondeTiles,
        .tilesCount    = 5,
        .color         = mondeColor,
        .map           = mondeMap,
        .mapWidth      = 120,
        .mapHeight     = 25,
        .chars         = mondeCharset,
        .color_back    = VCOL_BROWN,
        .color_back1   = VCOL_LT_BLUE,
        .color_back2   = VCOL_YELLOW,
        .color_border  = VCOL_LT_BLUE
    },
    /* Niveau 1 */
    {
        .enemies       = {{100, 0, 150, 0, false}, {250, 0, -150, 0, false}, {50, 0, 200, 0, false}},
        .enemyCount    = 3,
        .tileCollision = Monde2Collision,
        .tiles         = mondeTiles,
        .tilesCount    = 5,
        .color         = mondeColor,
        .map           = mondeMap,
        .mapWidth      = 120,
        .mapHeight     = 25,
        .chars         = mondeCharset,
        .color_back    = VCOL_BROWN,
        .color_back1   = VCOL_LT_BLUE,
        .color_back2   = VCOL_YELLOW,
        .color_border  = VCOL_LT_BLUE
    },
    /* Niveau 2 */
    {
        .enemies       = {{60, 0, 250,0,false}, {180, 0, -250,0,false}, {280, 0, 200,0,false}, {120, 0, -200,0,false}},
        .enemyCount    = 4,
        .tileCollision = Monde3Collision,
        .tiles         = mondeTiles,
        .tilesCount    = 5,
        .color         = mondeColor,
        .map           = mondeMap,
        .mapWidth      = 120,
        .mapHeight     = 25,
        .chars         = mondeCharset,
        .color_back    = VCOL_BROWN,
        .color_back1   = VCOL_LT_BLUE,
        .color_back2   = VCOL_YELLOW,
        .color_border  = VCOL_LT_BLUE
    }
};

/*=============================================================================
 *  Prototypes
 *============================================================================*/

void updatePlayer(void);
void spawnEnemy(int id, int x, int y, long vx);
void updateEnemies(void);
void updateLevelLogic(void);
void updateSprites(void);

void applyPatches(void);

static bool isSolidAtPixel(int px, int py);
static void drawMap(void);
static void init_sprites(void);
static bool checkCollisionAABB(int ax, int ay, int aw, int ah,
                               int bx, int by, int bw, int bh);
static bool enemyCanSeePlayer(Enemy *e);
static void playerTakeDamage(int knockbackDir);
static void drawHUD(void);
__interrupt static void irq_hud(void);
static void irq_logic(void);
__interrupt static void irq_music(void);
static void drawBottomPanel(void);
static void irq_bottom(void);
static void spawnLevelEnemies(void);

static void drawNumber(char *row, char *colorRow, int pos, int value, int digits);
void music_init(char subtune);
void music_play(void);
static void load_charpad_level(int levelIdx);
void tile_expand_map(char* map, const char* tiles);
static void resetEnemySpawnFlags(void);

/*=============================================================================
 *  Stubs (à implémenter)
 *============================================================================*/

void updateLevelLogic(void) {}
void applyPatches(void)     {}

/*=============================================================================
 *  Initialisation
 *============================================================================*/

static void init_sprites(void)
{    
    mmap_set(MMAP_RAM);
    mmap_set(MMAP_NO_BASIC);

    /* Copie des données du sprite en RAM sprite */
    memcpy(SpriteRAM, sprite_player, 64);
    memcpy(SpriteRAM + 64, sprite_enemy, 64);

    /* Efface l'écran texte */
    memset(Screen, 32, 1000);

    spr_init(Screen);

    /* Multicolore global activé pour tous les sprites */
    vic.spr_multi    = 0xFF;
    vic.spr_mcolor0  = VCOL_BLACK;;
    vic.spr_mcolor1  = VCOL_BROWN;

    sprite_base = (uint16_t)SpriteRAM / 64;
}

static void init_player(void)
{
    /*
     *  Coordonnée X hardware du sprite = playerX + 24 (offset VIC)
     *  spr_set gère correctement le bit 9 de X en interne.
     */
    spr_set(PLAYER_SPRITE,
            true,
            playerX + 24, playerY,
            sprite_base,
            VCOL_RED,
            true,   /* multicolore */
            false,  /* pas de double largeur */
            false); /* pas de double hauteur */
}

/*=============================================================================
 *  Logique joueur
 *============================================================================*/

static void playerTakeDamage(int knockbackDir)
{
    if (playerInvTimer > 0) return;   // déjà invincible

    playerHP--;
    hudDirty = 1;
    playerInvTimer = 60;              // 1 seconde d’invincibilité
    playerOnGround = false;

    // Knockback
    playerVy = -1200;
    playerVx = knockbackDir * 800;

    if (playerHP <= 0) {
        playerIsDead = true;
        respawnTimer = 60;
    }
}

void updatePlayer(void)
{
    /* Sortie par la droite → niveau suivant */
    if (playerX >= worldMaxX && !needLevelChange) {
        needLevelChange = true;
    }

    if (playerInvTimer > 0) {
        playerInvTimer--;
    }

    if (playerIsDead) {
        respawnTimer--;
        if (respawnTimer <= 0) {
            playerHP       = 3;
            playerScore    = 0;
            currentLevel   = 0;      /* ← retour au niveau 1 */
            hudDirty       = 1;
            playerInvTimer = 60;

            playerX  = 20;
            playerY  = 0;
            playerXf = (long)playerX << 8;
            playerYf = (long)playerY << 8;
            playerVx = 0;
            playerVy = 0;

            playerIsDead = false;

            currentPage = -1;

            /* Recharger le niveau 0 */
            load_charpad_level(0); /* puis la map */  
            drawBottomPanel();
            resetEnemySpawnFlags();
            spawnLevelEnemies();
        }
        return;
    }

    joy_poll(0);

    /* --- Accélération horizontale --- */
    if (joyx[0] < 0) {
        playerVx -= PLAYER_ACC_X;
        if (playerVx < -PLAYER_MAX_VX) playerVx = -PLAYER_MAX_VX;
    }
    else if (joyx[0] > 0) {
        playerVx += PLAYER_ACC_X;
        if (playerVx >  PLAYER_MAX_VX) playerVx =  PLAYER_MAX_VX;
    }
    else {
        /* Friction */
        if (playerVx > 0) {
            playerVx -= PLAYER_FRICTION;
            if (playerVx < 0) playerVx = 0;
        }
        else if (playerVx < 0) {
            playerVx += PLAYER_FRICTION;
            if (playerVx > 0) playerVx = 0;
        }
    }

    /* --- Saut (uniquement si au sol) --- */
    if (joyy[0] < 0 && playerOnGround) {
        playerVy = PLAYER_JUMP_VY;
        playerOnGround = false;
    }

    /* --- Gravité --- */
    playerVy += PLAYER_GRAVITY;
    if (playerVy > PLAYER_MAX_VY) playerVy = PLAYER_MAX_VY;

    /* --- Intégration sub-pixel --- */
    playerXf += playerVx;
    playerYf += playerVy;

    if (playerXf < ((long)WORLD_MIN_X << 8)) {
        playerXf = (long)WORLD_MIN_X << 8;
        playerVx = 0;
    }
    if (playerXf > ((long)worldMaxX << 8)) {
        playerXf = (long)worldMaxX << 8;
        playerVx = 0;
    }

    /* Conversion en pixels entiers */
    playerX = playerXf >> 8;
    playerY = playerYf >> 8;

    // --- COLLISION VERTICALE AVEC LA MAP ---
    if (playerVy > 0) {
        int feetY = (playerYf >> 8) + PLAYER_HEIGHT;

        if (isSolidAtPixel(playerX, feetY) ||
            isSolidAtPixel(playerX + PLAYER_WIDTH - 1, feetY)) {

            int tileY    = feetY / 8;
            int newFeetY = tileY * 8;      /* bord supérieur de la tile solide */

            playerY  = newFeetY - PLAYER_HEIGHT;
            playerYf = (long)playerY << 8;
            playerVy = 0;
            playerOnGround = true;
        }

        if (playerYf > ((long)WORLD_MAX_Y << 8)) {
            playerYf = (long)(MAP_H * 8 - PLAYER_HEIGHT) << 8;
            playerY        = playerYf >> 8;
            playerVy       = 0;
            playerOnGround = true;
        }
    }
    else if (playerVy < 0) {
        // collision tête avec plafond (plus tard)
    }

    // --- Auto-franchissement horizontal (safe) ---
    if (playerVx > 0) {
        int rightX = playerX + PLAYER_WIDTH - 1;

        // collision murale ?
        if (isSolidAtPixel(rightX, playerY + 4) ||
            isSolidAtPixel(rightX, playerY + PLAYER_HEIGHT - 4)) {

            // Conditions de sécurité
            if (playerVy > 0) goto blockRight;        // on tombe → pas de step-up
            if (playerY <= 2) goto blockRight;        // trop haut → pas de step-up
            if (isSolidAtPixel(rightX, playerY - 4)) goto blockRight; // plafond

            // Step-up 1 à 4 pixels
            for (int step = 1; step <= 4; step++) {
                if (!isSolidAtPixel(rightX, playerY - step)) {
                    playerY  -= step;
                    playerYf -= (step << 8);
                    goto endRight;
                }
            }

    blockRight:
            // Blocage normal
            playerX  = (rightX / 8) * 8 - PLAYER_WIDTH;
            playerXf = (long)playerX << 8;
            playerVx = 0;

    endRight:
            ;
        }
    }
    // Collision murale à gauche
    if (playerVx < 0) {
        int leftX = playerX;

        // collision murale ?
        if (isSolidAtPixel(leftX, playerY + 4) ||
            isSolidAtPixel(leftX, playerY + PLAYER_HEIGHT - 4)) {

            // Conditions de sécurité
            if (playerVy > 0) goto blockLeft;        // on tombe → pas de step-up
            if (playerY <= 2) goto blockLeft;        // trop haut → pas de step-up
            if (isSolidAtPixel(leftX, playerY - 4)) goto blockLeft; // plafond

            // Step-up 1 à 4 pixels
            for (int step = 1; step <= 4; step++) {
                if (!isSolidAtPixel(leftX, playerY - step)) {
                    playerY  -= step;
                    playerYf -= (step << 8);
                    goto endLeft;
                }
            }

    blockLeft:
            // Blocage normal
            playerX  = (leftX / 8 + 1) * 8;
            playerXf = (long)playerX << 8;
            playerVx = 0;

    endLeft:
            ;
        }
    }
}

/*=============================================================================
 *  Les ennemis
 *============================================================================*/
static Enemy enemies[MAX_ENEMIES];

void spawnEnemy(int id, int x, int y, long vx)
{
    Enemy *e = &enemies[id];

    e->x  = x;
    e->y  = y;
    e->xf = (long)x << 8;
    e->yf = (long)y << 8;
    e->vx = vx;
    e->vy = 0;   // ← ajouter cette ligne
    e->active = true;
    e->spriteId = id + 1;   // sprite 1, 2, 3, 4…
    e->page = levels[currentLevel].enemies[id].page;

    // Sprite VIC
    spr_set(e->spriteId,
            true,
            x + SCREEN_OFFSET_X,
            y + SCREEN_OFFSET_Y,
            sprite_base + 1,   // sprite ennemi = bloc suivant
            VCOL_WHITE,
            true, false, false);
}

void updateEnemies(void)
{
    for (int i = 0; i < MAX_ENEMIES; i++) {
        Enemy *e = &enemies[i];
        if (!e->active) continue;

        /* --- IA : décision direction --- */
        if (enemyCanSeePlayer(e)) {
            e->aiMode = AI_CHASE;
            e->vx = (playerX < e->x) ? -300 : 300;
        } else {
            e->aiMode = AI_PATROL;
            /* vx garde sa valeur de patrouille courante */
        }

        /* --- Gravité --- */
        e->vy += ENEMIES_GRAVITY;
        if (e->vy > ENEMIES_MAX_VY) e->vy = ENEMIES_MAX_VY;

        /* --- Intégration (toujours, quel que soit le mode) --- */
        e->xf += e->vx;
        e->yf += e->vy;
        e->x = e->xf >> 8;
        e->y = e->yf >> 8;

        /* --- Bord de plateforme → demi-tour (patrouille seulement) --- */
        if (e->aiMode == AI_PATROL) {
            int lookX   = e->x + ((e->vx > 0) ? ENEMIES_WIDTH : -1);
            int lookFeetY = e->y + ENEMIES_HEIGHT + 1;
            if (!isSolidAtPixel(lookX, lookFeetY)) {
                e->vx  = -e->vx;
                e->xf  = (long)e->x << 8;
            }
        }

        /* --- Collision verticale --- */
        int feetY = e->y + ENEMIES_HEIGHT;
        if (isSolidAtPixel(e->x, feetY) ||
            isSolidAtPixel(e->x + ENEMIES_WIDTH - 1, feetY)) {
            int newFeetY = (feetY / 8) * 8;
            e->y  = newFeetY - ENEMIES_HEIGHT;
            e->yf = (long)e->y << 8;
            e->vy = 0;
        }
        
        /* --- Franchissement optimisé (Marche de 8px / 1 Tile) --- */
        if (e->vx > 0) {
            int rightX = e->x + ENEMIES_WIDTH - 1;

            // Test obstacle au niveau du pied
            if (isSolidAtPixel(rightX, e->y + ENEMIES_HEIGHT - 1)) {

                // Test hauteur libre après montée
                if (!isSolidAtPixel(rightX, e->y - 8 + ENEMIES_HEIGHT - 1) &&
                    !isSolidAtPixel(rightX, e->y - 8 + ENEMIES_HEIGHT / 2)) {

                    // Succès : grimpe
                    e->y  -= 8;
                    e->yf  = (long)e->y << 8;

                    // Recalage X
                    //e->x   = (rightX / 8) * 8 - ENEMIES_WIDTH;
                    e->x = (rightX / 8) * 8 - ENEMIES_WIDTH - 1;
                    e->xf  = (long)e->x << 8;

                } else {
                    // Échec : demi-tour
                    e->vx = -e->vx;
                    e->x  = (rightX / 8) * 8 - ENEMIES_WIDTH;
                    e->xf = (long)e->x << 8;
                }
            }
        }
        else if (e->vx < 0) {
            int leftX = e->x;

            if (isSolidAtPixel(leftX, e->y + ENEMIES_HEIGHT - 1)) {

                if (!isSolidAtPixel(leftX, e->y - 8 + ENEMIES_HEIGHT - 1) &&
                    !isSolidAtPixel(leftX, e->y - 8 + ENEMIES_HEIGHT / 2)) {

                    e->y  -= 8;
                    e->yf  = (long)e->y << 8;

                    //e->x   = (leftX / 8 + 1) * 8;
                    e->x = (leftX / 8 + 1) * 8 + 1;
                    e->xf  = (long)e->x << 8;

                } else {
                    e->vx = -e->vx;
                    e->x  = (leftX / 8 + 1) * 8;
                    e->xf = (long)e->x << 8;
                }
            }
        }

        /* --- Garde-fou bas de map --- */
        if (e->yf > ((long)(WORLD_MAX_Y) << 8)) {
            e->yf = (long)(WORLD_MAX_Y) << 8;
            e->y  = e->yf >> 8;
            e->vy = 0;
        }

        /* --- Bords monde horizontaux --- */
        if (e->x < 0) {
            e->x = 0; e->xf = 0; e->vx = -e->vx;
        }
        if (e->x > worldMaxX) {
            e->x  = worldMaxX;
            e->xf = (long)e->x << 8;
            e->vx = -e->vx;
        }

        /* --- Collision joueur ↔ ennemi --- */
        if (!playerIsDead && playerInvTimer == 0) {
            if (checkCollisionAABB(playerX, playerY, PLAYER_WIDTH, PLAYER_HEIGHT,
                                e->x,    e->y,    ENEMIES_WIDTH, ENEMIES_HEIGHT)) {

                /* Le joueur tombe sur le dessus de l'ennemi ? */
                int playerFeetY  = playerY + PLAYER_HEIGHT;
                int enemyTopY    = e->y;
                bool fromAbove   = (playerVy > 0) &&
                                (playerFeetY <= enemyTopY + 4);  /* marge de 4px */

                if (fromAbove) {
                    /* Tuer l'ennemi */
                    e->active = false;
                    vic.spr_enable &= ~(1 << e->spriteId);
                    playerScore += 100;
                    hudDirty = 1;   /* redessiner le bandeau */

                    /* Rebond du joueur */
                    playerVy = PLAYER_JUMP_VY / 2;   /* demi-saut automatique */
                    playerOnGround = false;
                }
                else {
                    /* Contact latéral → dégâts */
                    int dir = (playerX < e->x) ? -1 : 1;
                    playerTakeDamage(dir);
                }
            }
        }        
    }
}

static bool enemyCanSeePlayer(Enemy *e)
{
    /* Même niveau vertical à ±8 pixels près */
    int dy = playerY - e->y;
    if (dy < -8 || dy > 8) return false;

    /* Distance horizontale max = 80 pixels */
    int dx = playerX - e->x;
    if (dx < -80 || dx > 80) return false;

    return true;
}

static void spawnLevelEnemies(void)
{
    /* Désactive tous les ennemis */
    for (int i = 0; i < MAX_ENEMIES; i++) {
        enemies[i].active = false;
        vic.spr_enable &= ~(1 << enemies[i].spriteId);
    }

    const LevelData *ld = &levels[currentLevel];

    /* Respawn selon le niveau */
    for (int i = 0; i < ld->enemyCount; i++) {
        if (ld->enemies[i].page == 0)
            spawnEnemy(i,
                    ld->enemies[i].x,
                    ld->enemies[i].y,
                    ld->enemies[i].vx);
    }
}

static void resetEnemySpawnFlags(void)
{
    LevelData *ld = &levels[currentLevel];
    for (int i = 0; i < ld->enemyCount; i++)
        ld->enemies[i].isAlreadySpawn = false;
}

/*=============================================================================
 *  Logique de collision
 *============================================================================*/
static void debugTileIndexBottom(int tileIndex, uint8_t coll)
{
    char *row      = Screen   + (24 * 40);
    char *colorRow = (char*)0xD800 + (24 * 40);

    int v = tileIndex;

    row[35] = '0' + (v / 10);
    row[36] = '0' + (v % 10);

    colorRow[35] = VCOL_CYAN;
    colorRow[36] = VCOL_CYAN;
}

void drawDebugColumns(void)
{
    for (int x = 0; x < 40; x++) {
        int px = x * 8;
        int cx = px / 8;

        for (int y = 1; y < 23; y++) {   // du haut au bas
            Screen[y * 40 + cx] = 29;   // tile plein
            ((char*)0xD800)[y * 40 + cx] = VCOL_RED;
        }
    }
}


static bool isSolidAtPixel(int px, int py)
{
    int tileX = px >> 3;
    int tileY = py >> 3;

    const LevelData *ld = &levels[currentLevel];

    if (tileX < 0 || tileX >= ld->mapWidth ||
        tileY < 0 || tileY >= ld->mapHeight)
        return false;

    uint8_t tileIndex = ld->map[tileY * ld->mapWidth + tileX];
    uint8_t coll      = ld->tileCollision[tileIndex];

    return (coll != 0);
}


static bool checkCollisionAABB(int ax, int ay, int aw, int ah,
                               int bx, int by, int bw, int bh)
{
    return !( ax + aw <= bx ||
              bx + bw <= ax ||
              ay + ah <= by ||
              by + bh <= ay );
}


/*=============================================================================
 *  Mise à jour des sprites VIC
 *
 *  Le registre vic.spr_pos.x est sur 8 bits ; pour les positions > 255 pixels
 *  il faut aussi positionner le bit correspondant dans vic.spr_ena_x (MSB X).
 *============================================================================*/

void updateSprites(void)
{
    // Conversion position monde → position écran
    int hwX = (playerX - viewX * 8) + SCREEN_OFFSET_X;
    int hwY = playerY + SCREEN_OFFSET_Y + (MAP_Y_OFF * 8);

    vic.spr_pos[PLAYER_SPRITE].x = (byte)(hwX & 0xFF);
    vic.spr_pos[PLAYER_SPRITE].y = (byte)(hwY);

    if (hwX >= 256)
        vic.spr_msbx |=  (1 << PLAYER_SPRITE);
    else
        vic.spr_msbx &= ~(1 << PLAYER_SPRITE);

    for (int i = 0; i < MAX_ENEMIES; i++) {
        Enemy *e = &enemies[i];
        if (!e->active) continue;

        int hwX = (e->x - viewX * 8) + SCREEN_OFFSET_X;
        int hwY = e->y + SCREEN_OFFSET_Y + (MAP_Y_OFF * 8);

        vic.spr_pos[e->spriteId].x = (byte)(hwX & 0xFF);
        vic.spr_pos[e->spriteId].y = (byte)(hwY);

        if (hwX >= 256)
            vic.spr_msbx |=  (1 << e->spriteId);
        else
            vic.spr_msbx &= ~(1 << e->spriteId);

        // Ennemi hors de la page visible → on le cache
        if (hwX < SCREEN_OFFSET_X || hwX > SCREEN_OFFSET_X + 320) {
            vic.spr_enable &= ~(1 << e->spriteId);
            continue;
        }
        vic.spr_enable |= (1 << e->spriteId);
    }

    /* Clignotement invincibilité */
    if (playerInvTimer > 0) {
        if ((playerInvTimer & 4) == 0)
            vic.spr_enable &= ~(1 << PLAYER_SPRITE);
        else
            vic.spr_enable |=  (1 << PLAYER_SPRITE);
    } else {
        vic.spr_enable |= (1 << PLAYER_SPRITE);
    }
}


/*=============================================================================
 *  HUD
 *============================================================================*/

static void drawHUD(void)
{
    char * const ColorRAM = (char *)0xD800;
    char *row = Screen;

    // Efface la ligne
    for (int i = 0; i < 40; i++) {
        row[i]     = CHAR_EMPTY;
        ColorRAM[i] = VCOL_CYAN;   /* fond noir = texte invisible */
    }

    // Affiche les cœurs en rouge
    for (int i = 0; i < playerHP; i++) {
        row[i]     = CHAR_HEART;
        ColorRAM[i] = VCOL_RED;     /* cœurs en rouge */
    }
}


/*=============================================================================
 *  BOTTOM PANEL
 *============================================================================*/

static void drawBottomPanel(void)
{
    char * const ColorRAM = (char *)0xD800;
    char *row      = Screen   + (24 * 40);
    char *colorRow = ColorRAM + (24 * 40);

    /* Efface la ligne */
    for (int i = 0; i < 40; i++) {
        row[i]      = CHAR_EMPTY;
        colorRow[i] = VCOL_LT_GREY;
    }

    /* Label "SCORE" */
    const char *label = "SCORE:";
    for (int i = 0; label[i]; i++) {
        char c = label[i];
        if (c >= 'A' && c <= 'Z') c = c - 'A' + 1;
        row[2 + i]      = c;
        colorRow[2 + i] = VCOL_WHITE;
    }

    /* Valeur du score sur 6 chiffres */
    drawNumber(row, colorRow, 9, playerScore, 6);

    /* Niveau actuel */
    const char *lvl = "LEVEL:";
    for (int i = 0; lvl[i]; i++) {
        char c = lvl[i];
        if (c >= 'A' && c <= 'Z') c = c - 'A' + 1;
        row[20 + i]      = c;
        colorRow[20 + i] = VCOL_WHITE;
    }
    drawNumber(row, colorRow, 27, currentLevel + 1, 1);

    /* --- DEBUG TILE INDEX --- */
    /*
    const char *dbg = "T:";
    for (int i = 0; dbg[i]; i++) {
        char c = dbg[i];
        if (c >= 'A' && c <= 'Z') c = c - 'A' + 1;
        row[33 + i]      = c;
        colorRow[33 + i] = VCOL_YELLOW;
    }
    */
}

static void drawNumber(char *row, char *colorRow, int pos, int value, int digits)
{
    for (int i = digits - 1; i >= 0; i--) {
        int digit = value % 10;
        value /= 10;
        char c = digit + '0';   /* '0' = 48 en ASCII mais... */
        /* Les chiffres ASCII 48-57 → codes écran C64 : 48-57 aussi, c'est bon ! */
        row[pos + i]      = c;
        colorRow[pos + i] = VCOL_YELLOW;
    }
}

/*=============================================================================
 *  MUSIC
 *============================================================================*/

void music_init(char subtune)
{
	__asm
	{
		lda		subtune
		jsr		$B318
	}
}

void music_play(void)
{
	__asm
	{
		jsr		$A006
	}
}


/*=============================================================================
 *  CHARPAD load
 *============================================================================*/

static void load_charpad_level(int levelIdx)
{  
    // Prépare l'écran de loading
    memset(Screen, 32, 1000);

    rirq_data(&hud, 2, VCOL_BLACK);
    rirq_data(&game, 0, 0x15);
    rirq_data(&game, 1, 0x08);
    rirq_data(&game, 2, VCOL_BLACK);
    rirq_data(&bottom, 2, VCOL_BLACK);

    vic.spr_enable = 0;    // tous les sprites sont disabled

    const char *msg = "LOADING...";
    int start = (40 - 10) / 2 + 40 * 12;

    for (int i = 0; msg[i]; i++) {
        char c = msg[i];
        if (c >= 'A' && c <= 'Z') c = c - 'A' + 1;
        Screen[start + i] = c;
        ColorRAM[start + i] = VCOL_WHITE;
    }

    vic.color_back   = VCOL_BLACK;
    vic.color_border = VCOL_BLACK;

    // Chargement

    const LevelData *ld = &levels[levelIdx];
    int bi = levelIdx * 4;

    // Lecture depuis disque
    flossiec_open(levelBlks[bi].track,   levelBlks[bi].sector);
    flossiec_read_lzo(ld->chars, 2048);
    flossiec_close();

    flossiec_open(levelBlks[bi+1].track, levelBlks[bi+1].sector);
    flossiec_read_lzo(ld->map, 3000);
    flossiec_close();  

    flossiec_open(levelBlks[bi+2].track, levelBlks[bi+2].sector);
    flossiec_read(ld->color, 256);
    flossiec_close();  

    flossiec_open(levelBlks[bi+3].track, levelBlks[bi+3].sector);
    flossiec_read(ld->tiles, ld->tilesCount);
    flossiec_close();
    
    // Fin du chargement, on bascule sur l'autre monde

    mmap_set(MMAP_RAM);      
    memcpy(Charset, ld->chars, 2048);
    mmap_set(MMAP_NO_BASIC);

    //tile_expand_map(ld->map, ld->tiles);

    //for (int i = 0; i < 1000; i++)
    //    ColorRAM[i] = ld->color[(uint8_t)Screen[i]] & 0x0F;

    vic.color_back   = ld->color_back;
    vic.color_back1  = ld->color_back1;
    vic.color_back2  = ld->color_back2;
    vic.color_border = ld->color_border;

    /* Met à jour l'IRQ game avec la bonne couleur de fond */
    rirq_data(&hud, 2, VCOL_LT_BLUE);
    rirq_data(&game, 0, 0x1e);
    rirq_data(&game, 1, 0x18);
    rirq_data(&game, 2, ld->color_back);
    rirq_data(&bottom, 2, VCOL_DARK_GREY);

    worldMaxX = ld->mapWidth * 8 - PLAYER_WIDTH;

    // On remet tous les sprites
    vic.spr_enable |= (1 << PLAYER_SPRITE);

    for (int i = 0; i < ld->enemyCount; i++)
        vic.spr_enable |= (1 << enemies[i].spriteId);
}

void tile_expand_map(char* map, const char* tiles)
{
    char *sp = Screen + 40;   // début map
    const char *mp = map;

    for (int i = 0; i < 40 * 23; i++) {
        sp[i] = tiles[(uint8_t)mp[i]];
    }
}

void drawVisibleMap(void)
{
    const LevelData *ld = &levels[currentLevel];
    int visibleRows = 25 - MAP_Y_OFF - 1;

    for (int y = 0; y < visibleRows; y++) {
        char *row      = Screen   + (y + MAP_Y_OFF) * 40;
        char *colorRow = ColorRAM + (y + MAP_Y_OFF) * 40;

        for (int x = 0; x < 40; x++) {
            int srcX = viewX + x;
            uint8_t tile    = (uint8_t)ld->map[y * ld->mapWidth + srcX];
            uint8_t charIdx = (uint8_t)ld->tiles[tile];  // tile → char

            row[x]      = charIdx;                        // char dans Screen
            colorRow[x] = ld->color[charIdx] & 0x0F;     // couleur du char
        }
    }
}


/*=============================================================================
 *  IRQ raster
 *============================================================================*/
__interrupt static void irq_music(void)
{
    music_play();
}

static void init_irq(void)
{
    rirq_init_kernal();

    rirq_build(&hud, 3);
    rirq_write(&hud, 0, &vic.memptr,     0x15);
    rirq_write(&hud, 1, &vic.ctrl2,      0x08);
    rirq_write(&hud, 2, &vic.color_back, VCOL_LT_BLUE);
    rirq_set(0, 48, &hud);
    
    rirq_build(&game, 3);
    rirq_write(&game, 0, &vic.memptr,     0x1e); // 0x1e = 7 * $800 = $3800 --> cf adresse chartset
    rirq_write(&game, 1, &vic.ctrl2,      0x18);
    rirq_write(&game, 2, &vic.color_back, VCOL_BROWN);
    rirq_set(1, 58, &game);
    
    rirq_build(&bottom, 3);
    rirq_write(&bottom, 0, &vic.memptr,     0x15);
    rirq_write(&bottom, 1, &vic.ctrl2,      0x08);
    rirq_write(&bottom, 2, &vic.color_back, VCOL_DARK_GREY);
    rirq_set(2, 242, &bottom);
    
    rirq_build(&rirq_music, 1);
    rirq_call(&rirq_music, 0, irq_music);
    rirq_set(3, 260, &rirq_music);

    rirq_sort();
    rirq_start();
}

// Après les flosskio_mapdir, avant tout chargement :
static void debug_blks(floss_blk *blks, int count)
{
    char *row      = Screen;
    char *colorRow = (char *)0xD800;

    for (int i = 0; i < count; i++) {
        // Track
        row[i * 6 + 0] = '0' + (blks[i].track / 10);
        row[i * 6 + 1] = '0' + (blks[i].track % 10);
        row[i * 6 + 2] = '/';
        // Sector
        row[i * 6 + 3] = '0' + (blks[i].sector / 10);
        row[i * 6 + 4] = '0' + (blks[i].sector % 10);
        row[i * 6 + 5] = ' ';

        colorRow[i * 6 + 0] = VCOL_YELLOW;
        colorRow[i * 6 + 1] = VCOL_YELLOW;
        colorRow[i * 6 + 2] = VCOL_WHITE;
        colorRow[i * 6 + 3] = VCOL_CYAN;
        colorRow[i * 6 + 4] = VCOL_CYAN;
        colorRow[i * 6 + 5] = VCOL_WHITE;
    }
}

/*=============================================================================
 *  Point d'entrée
 *============================================================================*/
int main(void)
{       
    mmap_trampoline();

    // Init fast loader
    flossiec_init(8);

    flossiec_mapdir(p"chars1,map1,colour1,tiles1,chars2,map2,colour2,tiles2,chars3,map3,colour3,tiles3", levelBlks);

    //debug_blks(levelBlks, 12);
    //for (;;) ;
    hudDirty = 1;

    init_sprites();
    init_player();

    vic_setmode(VICM_TEXT_MC, Screen, Charset);
    load_charpad_level(0);

    drawBottomPanel();
    spawnLevelEnemies();   /* ← remplace les spawnEnemy hardcodés */
    music_init(1);

    init_irq();  

    for (;;) {
        vic_waitFrame();

        updatePlayer();
        // Page = écran de 40 tiles où se trouve le joueur
        int playerTileX = playerX >> 3;
        int page = playerTileX / 40;          // 0, 1 ou 2
        if (page > 2) page = 2;              // sécurité

        if (page != currentPage) {
            currentPage = page;
            viewX = page * 40;
            drawVisibleMap();

            // Spawn les ennemis de cette page
            LevelData *ld = &levels[currentLevel];
            for (int i = 0; i < ld->enemyCount; i++) {
                if (ld->enemies[i].page == page && !ld->enemies[i].isAlreadySpawn) {
                    spawnEnemy(i, ld->enemies[i].x, ld->enemies[i].y, ld->enemies[i].vx);
                    ld->enemies[i].isAlreadySpawn = true;
                }
            }
        }
        updateEnemies();
        updateLevelLogic(); 
        updateSprites();
        //drawDebugColumns();        

        if (hudDirty) {
            drawHUD();
            drawBottomPanel();
            hudDirty = 0;
        }

        if (needLevelChange) {
            needLevelChange = false;
            hudDirty = 1;
            currentPage = -1;

            currentLevel++;
            if (currentLevel >= MAX_LEVELS)
                currentLevel = 0;

            playerX  = 8;
            playerY  = 0;
            playerXf = (long)playerX << 8;
            playerYf = (long)playerY << 8;
            playerVx = 0;
            playerVy = 0;
            playerOnGround = false;

            load_charpad_level(currentLevel); 
            drawBottomPanel();
            resetEnemySpawnFlags();
            spawnLevelEnemies();   /* ← respawn ennemis du nouveau niveau */
        }       
    }

    flossiec_shutdown();
}