/*=============================================================================
 *  Platformer C64 – oscar64
 *  Architecture : IRQ raster cadencé à la frame, logique séparée du rendu
 *============================================================================*/

#include <c64/joystick.h>
#include <c64/vic.h>
#include <c64/rasterirq.h>
#include <c64/sprites.h>
#include <c64/memmap.h>
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
 *  Données du sprite joueur  (24×21 pixels, multicolore échiqueté)
 *============================================================================*/

const uint8_t sprite_player[64] = {
    0b00000000,0b00000000,0b00000000,
    0b00000000,0b01010100,0b00000000,
    0b00000000,0b01010100,0b00000000,
    0b00000000,0b01010101,0b00000000,
    0b00000000,0b10101000,0b00000000,
    0b00000000,0b10101100,0b00000000,
    0b00000000,0b10101010,0b00000000,
    0b00000000,0b00101000,0b00000000,
    0b00000000,0b00110000,0b00000000,
    0b00000000,0b00110011,0b00000000,
    0b00000000,0b00111111,0b00000000,
    0b00000011,0b11110000,0b00000000,
    0b00000011,0b00110000,0b00000000,
    0b00000000,0b00111100,0b00000000,
    0b00000000,0b00111111,0b00000000,
    0b00000000,0b00110011,0b00000000,
    0b00000000,0b00110011,0b00000000,
    0b00000000,0b00110011,0b11000000,
    0b00000000,0b00110000,0b00000000,
    0b00000000,0b00110000,0b00000000,
    0b00000000,0b00111100,0b00000000,
    0x00
};

const uint8_t sprite_enemy[64] = {
    0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,
    0x00
};

/*=============================================================================
 *  Adresses mémoire
 *============================================================================*/

char * const Screen    = (char *)0x0400;  /* écran texte par défaut           */
char * const SpriteRAM = (char *)0x3000;  /* données sprites (hors BASIC)     */

static uint8_t sprite_base; /* index de bloc sprite (addr / 64)              */

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

/*=============================================================================
 *  Structs
 *============================================================================*/

typedef struct {
    int niveau;
} Level;

static Level level1;
static bool  needLevelChange = false;

typedef struct {
    int x, y;
    long xf, yf;
    long vx, vy;      // ← vy ajouté
    bool active;
    uint8_t spriteId;

    uint8_t aiMode;   // 0 = patrouille, 1 = poursuite
} Enemy;

typedef struct {
    uint8_t solidRows[8];
    uint8_t solidRowCount;
    struct {
        uint8_t row;
        uint8_t xStart;
        uint8_t xEnd;
    } platforms[4];
    uint8_t platformCount;
    struct {
        int x, y;
        int vx;
    } enemies[MAX_ENEMIES];       /* ← ajout */
    uint8_t enemyCount;           /* ← ajout */
} LevelData;


/*=============================================================================
 *  Données sur les niveaux du jeu
 *============================================================================*/

static int currentLevel = 0;

const LevelData levels[MAX_LEVELS] = {
    /* Niveau 0 */
    {
        .solidRows     = {20},
        .solidRowCount = 1,
        .platforms     = {{14, 10, 20}},
        .platformCount = 1,
        .enemies       = {{80, 0, 200}, {200, 0, -200}},
        .enemyCount    = 2
    },
    /* Niveau 1 */
    {
        .solidRows     = {20},
        .solidRowCount = 1,
        .platforms     = {{10, 5, 15}, {15, 25, 35}},
        .platformCount = 2,
        .enemies       = {{100, 0, 150}, {250, 0, -150}, {50, 0, 200}},
        .enemyCount    = 3
    },
    /* Niveau 2 */
    {
        .solidRows     = {20},
        .solidRowCount = 1,
        .platforms     = {{8, 2, 12}, {12, 18, 28}, {16, 30, 38}},
        .platformCount = 3,
        .enemies       = {{60, 0, 250}, {180, 0, -250}, {280, 0, 200}, {120, 0, -200}},
        .enemyCount    = 4
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
void loadLevel(Level *level);
void applyPatches(void);
static bool isSolidAtPixel(int px, int py);
static void drawMap(void);
static void init_sprites(void);
static bool checkCollisionAABB(int ax, int ay, int aw, int ah,
                               int bx, int by, int bw, int bh);
static bool enemyCanSeePlayer(Enemy *e);
static void playerTakeDamage(int knockbackDir);
static void drawHUD(void);
static void irq_hud(void);
static void drawBottomPanel(void);
static void irq_bottom(void);
static void spawnLevelEnemies(void);
static void drawNumber(char *row, char *colorRow, int pos, int value, int digits);

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
    mmap_trampoline();
    mmap_set(MMAP_RAM);
    mmap_set(MMAP_NO_BASIC);

    /* Copie des données du sprite en RAM sprite */
    memcpy(SpriteRAM, sprite_player, 64);
    memcpy(SpriteRAM + 64, sprite_enemy, 64);

    /* Efface l'écran texte */
    memset(Screen, 32, 1000);

    spr_init(Screen);

    vic.color_back   = VCOL_BLACK;
    vic.color_border = VCOL_BLACK;

    /* Multicolore global activé pour tous les sprites */
    vic.spr_multi    = 0xFF;
    vic.spr_mcolor0  = VCOL_RED;;
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
            VCOL_BLUE,
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
    if (playerX >= WORLD_MAX_X && !needLevelChange) {
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

            /* Recharger le niveau 0 */
            loadLevel(&level1);
            memset(Screen + MAP_Y_OFF * 40, CHAR_EMPTY, MAP_H * 40);
            drawMap();
            drawBottomPanel();
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
    if (playerXf > ((long)WORLD_MAX_X << 8)) {
        playerXf = (long)WORLD_MAX_X << 8;
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

    // Sprite VIC
    spr_set(e->spriteId,
            true,
            x + SCREEN_OFFSET_X,
            y + SCREEN_OFFSET_Y,
            sprite_base + 1,   // sprite ennemi = bloc suivant
            VCOL_RED,
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
        if (e->x > WORLD_MAX_X) {
            e->x  = WORLD_MAX_X;
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
        spawnEnemy(i,
                   ld->enemies[i].x,
                   ld->enemies[i].y,
                   ld->enemies[i].vx);
    }
}

/*=============================================================================
 *  Logique de collision
 *============================================================================*/
static uint8_t collisionMap[MAP_H][MAP_W];

static bool isSolidAtPixel(int px, int py)
{
    if (py < 0) return false;
    int tx = px / 8;
    int ty = py / 8;

    if (tx < 0 || tx >= MAP_W || ty < 0 || ty >= MAP_H)
        return false;   // ← tu sors probablement ici !

    return (collisionMap[ty][tx] == TILE_SOLID);
}

static void drawMap(void)
{
    for (int ty = 0; ty < MAP_H; ++ty) {
        for (int tx = 0; tx < MAP_W; ++tx) {
            char c = (collisionMap[ty][tx] == TILE_SOLID) ? CHAR_SOLID : CHAR_EMPTY;
            Screen[(ty + MAP_Y_OFF) * MAP_W + tx] = c;
        }
    }
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
    int hwX = playerX + SCREEN_OFFSET_X;
    int hwY = playerY + SCREEN_OFFSET_Y + (MAP_Y_OFF * 8);  /* +8 pour la ligne HUD */

    vic.spr_pos[PLAYER_SPRITE].x = (byte)(hwX & 0xFF);
    vic.spr_pos[PLAYER_SPRITE].y = (byte)(hwY);

    if (hwX >= 256)
        vic.spr_msbx |=  (1 << PLAYER_SPRITE);
    else
        vic.spr_msbx &= ~(1 << PLAYER_SPRITE);


    for (int i = 0; i < MAX_ENEMIES; i++) {
        Enemy *e = &enemies[i];
        if (!e->active) continue;

        int hwX = e->x + SCREEN_OFFSET_X;
        int hwY = e->y + SCREEN_OFFSET_Y + (MAP_Y_OFF * 8);  /* +8 ici aussi */

        vic.spr_pos[e->spriteId].x = (byte)(hwX & 0xFF);
        vic.spr_pos[e->spriteId].y = (byte)(hwY);

        if (hwX >= 256)
            vic.spr_msbx |=  (1 << e->spriteId);
        else
            vic.spr_msbx &= ~(1 << e->spriteId);
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
        ColorRAM[i] = VCOL_BLACK;   /* fond noir = texte invisible */
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
    char *row      = Screen   + (23 * 40);
    char *colorRow = ColorRAM + (23 * 40);

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
 *  CHARGEMENT DYNAMIQUE DES NIVEAUX
 *============================================================================*/

 void loadLevel(Level *level)
{
    (void)level;

    const LevelData *ld = &levels[currentLevel];

    memset(collisionMap, TILE_EMPTY, sizeof(collisionMap));

    /* Sol */
    for (int r = 0; r < ld->solidRowCount; r++)
        for (int x = 0; x < MAP_W; x++)
            collisionMap[ld->solidRows[r]][x] = TILE_SOLID;

    /* Plateformes */
    for (int p = 0; p < ld->platformCount; p++)
        for (int x = ld->platforms[p].xStart; x < ld->platforms[p].xEnd; x++)
            collisionMap[ld->platforms[p].row][x] = TILE_SOLID;
}

/*=============================================================================
 *  IRQ raster
 *============================================================================*/

static void irq_hud(void)
{
    vic.color_back = VCOL_LT_GREY;
}

static void irq_bottom(void)
{
    vic.color_back = VCOL_DARK_GREY;
    //vic.color_border = VCOL_DARK_GREY;
}

static void irq_logic(void)
{
    vic.color_back = VCOL_WHITE;
    vic.color_border = VCOL_BLACK;
}

static void init_irq(void)
{
    rirq_init(true);    

    // --- IRQ HUD (ligne 30) ---
    RIRQCode *hud = rirq_alloc(2);
    rirq_build(hud, 2);
    rirq_call(hud, 1, irq_hud);
    rirq_set(0, 30, hud);

    // --- IRQ bandeau bas (ligne 215) ---
    RIRQCode *bottom = rirq_alloc(2);
    rirq_build(bottom, 2);
    rirq_call(bottom, 1, irq_bottom);
    rirq_set(1, 225, bottom);

    // --- IRQ logique du jeu (ligne 240) ---
    RIRQCode *logic = rirq_alloc(2);
    rirq_build(logic, 2);
    rirq_call(logic, 1, irq_logic);
    rirq_set(2, 250, logic);

    rirq_sort();
    rirq_start();
}

/*=============================================================================
 *  Point d'entrée
 *============================================================================*/

int main(void)
{
    hudDirty = 1;
    init_sprites();
    init_player();
    loadLevel(&level1);
    drawMap();
    drawBottomPanel();
    spawnLevelEnemies();   /* ← remplace les spawnEnemy hardcodés */
    init_irq();

    for (;;) {
        vic_waitFrame();

        updatePlayer();
        updateEnemies();
        updateLevelLogic();
        updateSprites();

        if (hudDirty) {
            drawHUD();
            drawBottomPanel();
            hudDirty = 0;
        }

        if (needLevelChange) {
            needLevelChange = false;
            hudDirty = 1;

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

            loadLevel(&level1);
            memset(Screen + MAP_Y_OFF * 40, CHAR_EMPTY, MAP_H * 40);
            drawMap();
            drawBottomPanel();
            spawnLevelEnemies();   /* ← respawn ennemis du nouveau niveau */
        }
    }
}