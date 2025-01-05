/*******************************************************************************************
*
*   raylib [core] example - Basic window
*
*   Welcome to raylib!
*
*   To test examples, just press Shift+F10 for Android Studio.
*
*   raylib official webpage: www.raylib.com
*
*   Enjoy using raylib. :)
*
*   Example licensed under an unmodified zlib/libpng license, which is an OSI-certified,
*   BSD-like license that allows static linking with closed source software
*
*   Copyright (c) 2013-2023 Ramon Santamaria (@raysan5) and reviewed by Victor Le Juez
*
********************************************************************************************/

#include <android/log.h>
#include <android/input.h>

#include "raylib.h"
#include "raymath.h"
#include "raymob.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <time.h>

#define BUF_SIZE 256

#define MY_LOG_TAG "UR_MOM"

#define LOG_INFO(...) do { __android_log_print(ANDROID_LOG_INFO, MY_LOG_TAG, __VA_ARGS__); } while(0)
#define LOG_DEBUG(...) do { __android_log_print(ANDROID_LOG_DEBUG, MY_LOG_TAG, __VA_ARGS__); } while(0)

#define TARGET_FPS 60
#define TABLEAU_PAD 0.008f
#define TABLEAU_MARGIN 0.012f
#define TABLEAU_Y_START 0.25f
#define TABLEAU_TOP_MARGIN 0.02f
#define CARD_SPLAY 0.15f // in % of card_height
#define TALON_SPLAY 0.3f 
#define CARD_VEL 2.0f // in % screen/s ?

enum face {
    FACE_ACE = 1,
    FACE_JACK = 11,
    FACE_QUEEN = 12,
    FACE_KING = 13,
    FACE_COUNT,
};
const char *faceNames[] = {
    [FACE_ACE]   = "ace",
    [FACE_JACK]  = "jack",
    [FACE_QUEEN] = "queen",
    [FACE_KING]  = "king",
};

enum suit {
    HEARTS,
    CLUBS,
    SPADES,
    DIAMONDS,
    SUIT_COUNT,
};

const char *suitNames[] = {
    [HEARTS]   = "hearts",
    [CLUBS]    = "clubs",
    [SPADES]   = "spades",
    [DIAMONDS] = "diamonds",
};

typedef struct {
    int value;
    enum suit suit;
    Vector2 pos;
    bool revealed;
} Card;

static inline bool is_black(Card c)
{
    return c.suit == SPADES || c.suit == CLUBS;
}

static inline bool is_red(Card c)
{
    return !is_black(c);
}

typedef struct {
    Card cards[52]; // size pile big enough to hold entire deck
    int count;
} Pile;

void pile_append(Pile *pile, Card card) {
    assert(pile->count < 52);
    pile->cards[pile->count++] = card;
}

void pile_append_many(Pile *dst, const Pile *src) {
    assert(dst->count + src->count < 52);
    memcpy(dst->cards+dst->count, src->cards, src->count*sizeof(*src->cards));
    dst->count += src->count;
}

Card pile_pop(Pile *pile) {
    assert(pile->count > 0);
    return pile->cards[--pile->count];
}

Card pile_peek(const Pile *pile) {
    assert(pile->count > 0);
    int idx = pile->count - 1;
    return pile->cards[idx];
}

Card pile_first(const Pile *pile) {
    assert(pile->count > 0);
    return pile->cards[0];
}

void pile_split(Pile *dst, Pile *src, size_t split)
{
    assert(split < src->count);
    size_t count = src->count - split;
    memcpy(dst->cards, src->cards+split, count*sizeof(*src->cards));
    dst->count = count;
    src->count = split;
}

#define TABLEAU_COLS 7
#define FOUNDATION_COLS 4
typedef struct {
    Pile tableau[TABLEAU_COLS];
    Pile foundation[FOUNDATION_COLS];
    Pile talon;
    Pile reserve;
} Game;

typedef struct {
    Pile pile;
    Vector2 start_pos;
    Vector2 end_pos;
    Pile *target;
    float t;
} InFlightPile;

// Global state
static Game game = {0};
static Texture2D cardTextures[14][4];
static Texture2D cardBack;
static InFlightPile pile_in_flight;
static bool in_flight = false;

// useful global vars
static int card_width_px;
static int card_height_px;
static float card_width;
static float card_height;
static float card_scale;
static Vector2 screen_dim;

Vector2 getFloatingPilePos(Vector2 root, size_t col) {
    return CLITERAL(Vector2) {
        root.x,
        root.y + col*CARD_SPLAY
    };
}

Vector2 getFoundationPos(size_t col)
{
    Vector2 root_pos = {TABLEAU_MARGIN, TABLEAU_Y_START-(card_height+TABLEAU_TOP_MARGIN)};
    return CLITERAL(Vector2) {
        root_pos.x + col*(card_width+TABLEAU_PAD),
        root_pos.y
    };
}

Vector2 getTableauPos(size_t col, size_t row)
{
    static Vector2 root_pos = {TABLEAU_MARGIN, TABLEAU_Y_START};
    return CLITERAL(Vector2) {
        root_pos.x + col*(card_width+TABLEAU_PAD),
        root_pos.y+card_height*0.15*row,
    };
}

bool getMoveTarget(Card c, InFlightPile *in_flight)
{
    in_flight->start_pos = c.pos;
    in_flight->t = 0.0f;
    // check if move to foundation is valid
    for (size_t i = 0; i < FOUNDATION_COLS; i++) {
        Pile *p = &game.foundation[i];
        in_flight->target = p;
        in_flight->end_pos = getFoundationPos(i);
        if (p->count == 0) {
            if (c.value == FACE_ACE) {
                return true;
            }
        } else {
            Card last = pile_peek(p);
            if (c.suit == last.suit && c.value == last.value + 1) {
                return true;
            }
        }
    }

    // check if move to tableau is valid
    for (size_t i = 0; i < TABLEAU_COLS; i++) {
        Pile *p = &game.tableau[i];
        in_flight->target = p;
        in_flight->end_pos = getTableauPos(i, p->count);
        if (p->count == 0) {
            if (c.value == FACE_KING) {
                return true;
            }
        } else {
            Card last = pile_peek(p);
            bool different = is_black(c) ^ is_black(last);
            if (different && c.value == last.value - 1) {
                return true;
            }
        }
    }
    return false;
}

void loadTextures() {
    //   all number card textures
    char texName[BUF_SIZE];
    for (int cardNum = 2; cardNum < 11; cardNum++) {
        for (enum suit suit = 0; suit < SUIT_COUNT; suit++) {
            snprintf(texName, BUF_SIZE, "playing-cards/%d_of_%s.png", cardNum, suitNames[suit]);
            cardTextures[cardNum][suit] = LoadTexture(texName);
        }
    }

    //   all face card textures
    //     Ace
    for (int suit = 0; suit < SUIT_COUNT; suit++) {
        snprintf(texName, BUF_SIZE, "playing-cards/ace_of_%s.png", suitNames[suit]);
        cardTextures[FACE_ACE][suit] = LoadTexture(texName);
    }
    //     Jack, Queen, King
    for (int cardNum = FACE_JACK; cardNum <= FACE_KING; cardNum++) {
        for (int suit = 0; suit < SUIT_COUNT; suit++) {
            snprintf(texName, BUF_SIZE, "playing-cards/%s_of_%s.png", faceNames[cardNum], suitNames[suit]);
            LOG_DEBUG("Trying to load tex from file: %s", texName);
            cardTextures[cardNum][suit] = LoadTexture(texName);
        }
    }
    //   card back
    Image image = LoadImage("playing-cards/card_back.png");
    cardBack = LoadTextureFromImage(image);
    card_width_px = image.width;
    card_height_px = image.height;
    UnloadImage(image);
}

static void renderCard(Card c) {
    Texture2D texture = c.revealed ? cardTextures[c.value][c.suit] : cardBack;
    DrawTextureEx(texture, Vector2Multiply(c.pos, screen_dim), 0.0f, card_scale, WHITE);
}

void renderTableau(void)
{
    for (size_t i = 0; i < TABLEAU_COLS; i++) {
        const Pile *p = &game.tableau[i];
        for (size_t j = 0; j < p->count; j++) {
            renderCard(p->cards[j]);
        }
    }
}

void renderFoundation(void)
{
    Vector2 root_pos = {TABLEAU_MARGIN, TABLEAU_Y_START-(card_height+TABLEAU_TOP_MARGIN)};
    for (size_t i = 0; i < FOUNDATION_COLS; i++) {
        Pile *p = &game.foundation[i];
        Vector2 placeholder_pos = {root_pos.x + i*(card_width+TABLEAU_PAD), root_pos.y};
        if (p->count > 0) {
            Card c = pile_peek(p);
            renderCard(c);
        } else {
            Vector2 size = {card_width * screen_dim.x, card_height * screen_dim.y};
            DrawRectangleV(Vector2Multiply(placeholder_pos, screen_dim), size, RED);
        }
    }
}

static void update(void)
{
    // update in-flight card positions
    if (in_flight) {
        float t_total = Vector2Distance(pile_in_flight.start_pos, pile_in_flight.end_pos) / CARD_VEL;
        Vector2 pile_root = Vector2Lerp(pile_in_flight.start_pos, pile_in_flight.end_pos, pile_in_flight.t);
        for (size_t i = 0; i < pile_in_flight.pile.count; i++) {
            pile_in_flight.pile.cards[i].pos = CLITERAL(Vector2) {
                .x = pile_root.x,
                .y = pile_root.y+i*CARD_SPLAY*card_height,
            };
        }
        pile_in_flight.t += CARD_VEL*GetFrameTime()*(1/t_total);
        if (pile_in_flight.t > 1.0f) {
            in_flight = false;
            pile_append_many(pile_in_flight.target, &pile_in_flight.pile);
        }
    }

    Vector2 touch_pos = Vector2Divide(GetTouchPosition(0), screen_dim);

    // update tableau card positions
    Vector2 root_pos = {TABLEAU_MARGIN, TABLEAU_Y_START};
    for (size_t i = 0; i < TABLEAU_COLS; i++) {
        Pile *p = &game.tableau[i];
        if (p->count > 0) {
            p->cards[p->count-1].revealed = true;
        }
        for (size_t j = 0; j < p->count; j++) {
            p->cards[j].pos = getTableauPos(i, j);
            // check if move can be made
            if (!in_flight) {
                Vector2 pos = p->cards[j].pos;
                bool last = j == p->count-1;
                float height = j == p->count-1 ? card_height : card_height*CARD_SPLAY;
                Rectangle collision_box = {
                    pos.x,
                    pos.y,
                    card_width,
                    height
                };

                bool pressed = IsMouseButtonPressed(0) && CheckCollisionPointRec(touch_pos, collision_box);
                if (pressed && getMoveTarget(p->cards[j], &pile_in_flight) && p->cards[j].revealed) {
                    pile_split(&pile_in_flight.pile, p, j);
                    in_flight = true;
                }
            }
        }
    }

    // update foundation card positions
    for (size_t i = 0; i < FOUNDATION_COLS; i++) {
        Pile *p = &game.foundation[i];
        Vector2 card_pos = getFoundationPos(i);
        for (size_t j = 0; j < p->count; j++) {
            p->cards[j].pos = card_pos;
        }
    }

    // update reserve
    if (game.reserve.count > 0) {
        Card c = pile_peek(&game.reserve);
        Rectangle collision_box = {
            c.pos.x,
            c.pos.y,
            card_width,
            card_height
        };
        if (IsMouseButtonPressed(0) && CheckCollisionPointRec(touch_pos, collision_box)) {
            // move to talon
            (void) pile_pop(&game.reserve);
            c.revealed = true;
            pile_append(&game.talon, c);
        }
        for (size_t i = 0; i < game.reserve.count; i++) {
            game.reserve.cards[i].pos = CLITERAL(Vector2) {
                1.0 - card_width - TABLEAU_MARGIN,
                TABLEAU_Y_START-(card_height+TABLEAU_TOP_MARGIN)
            };
            game.reserve.cards[i].revealed = false;
        }
    } else {
        // put everything in talon back into reserve
        if (game.talon.count > 0) {
            pile_split(&game.reserve, &game.talon, 0);
        }
    }

    // update talon
    if (game.talon.count > 0) {
        Vector2 talon_root = {
            .x = 1.0 - card_width*3 - TABLEAU_MARGIN,
            .y = TABLEAU_Y_START - (card_height+TABLEAU_TOP_MARGIN)
        };
        int start = game.talon.count-3;
        if (start < 0) start = 0;
        for (size_t i = start; i < game.talon.count; i++) {
            game.talon.cards[i].pos = CLITERAL(Vector2) {
                .x = talon_root.x + (i-start)*TALON_SPLAY*card_width,
                .y = talon_root.y
            };
        }
        if (!in_flight) {
            Card c = pile_peek(&game.talon);
            Rectangle collision_box = {
                .x = c.pos.x,
                .y = c.pos.y,
                .width = card_width,
                .height = card_height,
            };
            bool pressed = IsMouseButtonPressed(0) && CheckCollisionPointRec(touch_pos, collision_box);
            if (pressed && getMoveTarget(c, &pile_in_flight)) {
                (void) pile_pop(&game.talon);
                pile_in_flight.pile.count = 0;
                pile_append(&pile_in_flight.pile, c);
                in_flight = true;
            }
        }
    }
}

void render(void)
{
    DrawText("Welcome to Solitaire!", 190, 200, 32, RAYWHITE);

    renderTableau();

    renderFoundation();

    // reserve
    if (game.reserve.count > 0) {
        renderCard(pile_peek(&game.reserve));
    }
    // talon
    int start = game.talon.count-3;
    if (start < 0) start = 0;
    for (size_t i = start; i < game.talon.count; i++) {
        renderCard(game.talon.cards[i]);
    }
    // in flight cards
    if (in_flight) {
        for (size_t i = 0; i < pile_in_flight.pile.count; i++) {
            renderCard(pile_in_flight.pile.cards[i]);
        }
    }
}
//------------------------------------------------------------------------------------
// Program main entry point
//------------------------------------------------------------------------------------
int main(void)
{
    // Initialization
    //--------------------------------------------------------------------------------------
    InitWindow(0, 0, "raylib [core] example - basic window");
    SetTargetFPS(TARGET_FPS);   // Set our game to run at 60 frames-per-second
    screen_dim = CLITERAL(Vector2) {GetScreenWidth(), GetScreenHeight()};
    //--------------------------------------------------------------------------------------

    // Initialize game state
    //--------------------------------------------------------------------------------------
    srand(time(0));
    Pile deck = {0};
    // Init deck
    for (int i = 0; i < 13; i++) {
        for (int j = 0; j < SUIT_COUNT; j++) {
            Card c = {
                .value = i+1,
                .suit = j,
                .pos = {0},
                .revealed = false,
            };
            pile_append(&deck, c);
        }
    }

    // Shuffle deck (Fisher-Yates algorithm)
    for (int i = 51; i > 0; i--) {
        int r = rand() % i+1; // card to swap with
        Card tmp = deck.cards[r];
        deck.cards[r] = deck.cards[i];
        deck.cards[i] = tmp;
    }

    // Deal cards to foundation
    for (int i = 0; i < TABLEAU_COLS; i++) {
        for (int j = 0; j < i+1; j++) {
            pile_append(&game.tableau[i], pile_pop(&deck));
        }
    }

    // Deal remaining to reserve
    for (int i = deck.count-1; i >= 0; i--) {
        pile_append(&game.reserve, pile_pop(&deck));
    }
    //--------------------------------------------------------------------------------------

    // Loading Textures
    //--------------------------------------------------------------------------------------
    loadTextures();
    //--------------------------------------------------------------------------------------

    card_width = (1.0 - TABLEAU_PAD*6 - TABLEAU_MARGIN*2) / 7;
    int card_width_desired_px = (int) (card_width * screen_dim.x);
    card_scale = (float) card_width_desired_px / card_width_px;
    int card_width_scaled = card_width_px * card_scale;
    int card_height_scaled = card_height_px * card_scale;
    card_height = card_height_scaled / screen_dim.y;

    /* bool grabbed = false; */
    /* int hold_frame_count = 0; */
    /* Vector2 pos = {0.5-(card_width/2), 0.5-(card_height/2)}; */
    /* Vector2 touch_pos; */

    // Main game loop
    while (!WindowShouldClose())
    {
        update();

        BeginDrawing();
        ClearBackground(DARKGREEN);
        render();
        EndDrawing();
    }

    // De-Initialization
    //--------------------------------------------------------------------------------------
    for (int cardNum = 1; cardNum < 14; cardNum++) {
        for (int suit = 0; suit < SUIT_COUNT; suit++) {
            UnloadTexture(cardTextures[cardNum][suit]);
        }
    }
    CloseWindow();        // Close window and OpenGL context
    //--------------------------------------------------------------------------------------

    return 0;
}
