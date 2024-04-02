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

#include <android/asset_manager.h>
#include <android/imagedecoder.h>
#include <android/log.h>
#include <android/input.h>

#include "raylib.h"
#include "raymath.h"

#include <stdio.h>
#include <stdlib.h>
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
} Card;

typedef struct {
    Card cards[52]; // size pile big enough to hold entire deck
    int count;
} Pile;

void pile_append(Pile *pile, Card card) {
    assert(pile->count < 52);
    pile->cards[pile->count++] = card;
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

typedef struct {
    Pile tableau[7];
    Pile foundation[4];
    Pile talon;
    Pile reserve;
} Game;

// Global state
static Game game = {0};
static Texture2D cardTextures[14][4];
static Texture2D cardBack;

// useful global vars
static int card_width_px;
static int card_height_px;
static float card_width;
static float card_height;
static float card_scale;
static Vector2 screen_dim;

Image ImageFromAndroid(AAssetManager *assman, const char *filePath) {
    // Get the image from asset manager
    AAsset *pAndroidImage = AAssetManager_open(
            assman,
            filePath,
            AASSET_MODE_BUFFER);

    // Make a decoder to turn it into a texture
    AImageDecoder *pAndroidDecoder = NULL;
    int result = AImageDecoder_createFromAAsset(pAndroidImage, &pAndroidDecoder);
    assert(result == ANDROID_IMAGE_DECODER_SUCCESS);

    // make sure we get 8 bits per channel out. RGBA order.
    AImageDecoder_setAndroidBitmapFormat(pAndroidDecoder, ANDROID_BITMAP_FORMAT_RGBA_8888);

    // Get the image header, to help set everything up
    const AImageDecoderHeaderInfo *pAndroidHeader = NULL;
    pAndroidHeader = AImageDecoder_getHeaderInfo(pAndroidDecoder);

    // important metrics for sending to GL
    int32_t width = AImageDecoderHeaderInfo_getWidth(pAndroidHeader);
    int32_t height = AImageDecoderHeaderInfo_getHeight(pAndroidHeader);
    size_t stride = AImageDecoder_getMinimumStride(pAndroidDecoder);

    // Get the bitmap data of the image
    int buf_size = height * stride;
    uint8_t *imageData = malloc(buf_size);
    int decodeResult = AImageDecoder_decodeImage(
            pAndroidDecoder,
            imageData,
            stride,
            buf_size);
    assert(decodeResult == ANDROID_IMAGE_DECODER_SUCCESS);

    // Cleanup helpers
    AImageDecoder_delete(pAndroidDecoder);
    AAsset_close(pAndroidImage);

    Image image = {
        .data = imageData,
        .height = height,
        .width = width,
        .mipmaps = 1,
        .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8,
    };
    return image;
}

Texture2D LoadTextureFromAndroid(AAssetManager *assman, const char *filePath) {
    Image image = ImageFromAndroid(assman, filePath);
    Texture2D tex = LoadTextureFromImage(image);
    UnloadImage(image);
    return tex;
}

void loadTextures(AAssetManager *assman) {
    //   all number card textures
    char texName[BUF_SIZE];
    for (int cardNum = 2; cardNum < 11; cardNum++) {
        for (enum suit suit = 0; suit < SUIT_COUNT; suit++) {
            snprintf(texName, BUF_SIZE, "playing-cards/%d_of_%s.png", cardNum, suitNames[suit]);
            cardTextures[cardNum][suit] = LoadTextureFromAndroid(assman, texName);
        }
    }

    //   all face card textures
    //     Ace
    for (int suit = 0; suit < SUIT_COUNT; suit++) {
        snprintf(texName, BUF_SIZE, "playing-cards/ace_of_%s.png", suitNames[suit]);
        cardTextures[FACE_ACE][suit] = LoadTextureFromAndroid(assman, texName);
    }
    //     Jack, Queen, King
    for (int cardNum = FACE_JACK; cardNum <= FACE_KING; cardNum++) {
        for (int suit = 0; suit < SUIT_COUNT; suit++) {
            snprintf(texName, BUF_SIZE, "playing-cards/%s_of_%s.png", faceNames[cardNum], suitNames[suit]);
            LOG_DEBUG("Trying to load tex from file: %s", texName);
            cardTextures[cardNum][suit] = LoadTextureFromAndroid(assman, texName);
        }
    }
    //   card back
    Image image = ImageFromAndroid(assman, "playing-cards/card_back.png");
    cardBack = LoadTextureFromImage(image);
    card_width_px = image.width;
    card_height_px = image.height;
    UnloadImage(image);
}

void renderTableau()
{
    Vector2 root_pos = {TABLEAU_MARGIN, TABLEAU_Y_START};
    for (size_t i = 0; i < 7; i++) {
        Pile p = game.tableau[i];
        size_t j;
        Vector2 card_pos = {root_pos.x + i*(card_width+TABLEAU_PAD), root_pos.y};
        if (p.count > 0) {
            for (j = 0; j < p.count-1; j++) {
                DrawTextureEx(cardBack, Vector2Multiply(card_pos, screen_dim), 0.0f, card_scale, WHITE);
                card_pos.y += card_height*0.15;
            }
            Card c = pile_peek(&p);
            DrawTextureEx(cardTextures[c.value][c.suit], Vector2Multiply(card_pos, screen_dim), 0.0f, card_scale, WHITE);
        }
    }
}

void renderFoundation(void)
{
    Vector2 root_pos = {TABLEAU_MARGIN, TABLEAU_Y_START-(card_height+TABLEAU_TOP_MARGIN)};
    for (size_t i = 0; i < 4; i++) {
        Pile p = game.foundation[i];
        Vector2 card_pos = {root_pos.x + i*(card_width+TABLEAU_PAD), root_pos.y};
        if (p.count > 0) {
            Card c = pile_peek(&p);
            DrawTextureEx(cardTextures[c.value][c.suit], Vector2Multiply(card_pos, screen_dim), 0.0f, card_scale, WHITE);
        } else {
            Vector2 size = {card_width * screen_dim.x, card_height * screen_dim.y};
            DrawRectangleV(Vector2Multiply(card_pos, screen_dim), size, RED);
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
    screen_dim = (Vector2) {GetScreenWidth(), GetScreenHeight()};
    struct android_app *app = GetAndroidApp();
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
    for (int i = 0; i < 7; i++) {
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
    AAssetManager *assman = app->activity->assetManager;
    loadTextures(assman);
    //--------------------------------------------------------------------------------------

    card_width = (1.0 - TABLEAU_PAD*6 - TABLEAU_MARGIN*2) / 7;
    int card_width_desired_px = (int) (card_width * screen_dim.x);
    card_scale = (float) card_width_desired_px / card_width_px;
    int card_width_scaled = card_width_px * card_scale;
    int card_height_scaled = card_height_px * card_scale;
    card_height = card_height_scaled / screen_dim.y;

    bool grabbed = false;
    int hold_frame_count = 0;
    Vector2 touch_pos;
    Vector2 pos = {0.5-(card_width/2), 0.5-(card_height/2)};

    Rectangle collisionBox;
    // Main game loop
    while (!WindowShouldClose())    // Detect window close button or ESC key
    {
        // Update
        //----------------------------------------------------------------------------------
        collisionBox = (Rectangle){
                .x = pos.x,
                .y = pos.y,
                .width = card_width,
                .height = card_height,
        };
        //----------------------------------------------------------------------------------

        // Handle Input
        //----------------------------------------------------------------------------------
        touch_pos = Vector2Divide(GetTouchPosition(0), screen_dim);
        if (IsMouseButtonPressed(0)) {
            LOG_DEBUG("Pressed!");
        }
        if (IsMouseButtonDown(0) && CheckCollisionPointRec(touch_pos, collisionBox)) {
            LOG_INFO("YAY! WE'RE TOUCHING :) %d", hold_frame_count);
            hold_frame_count += 1;
        }
        if (IsMouseButtonReleased(0) && grabbed) {
            LOG_DEBUG("RELEASED!");
            hold_frame_count = 0;
            grabbed = false;
        }
        if (hold_frame_count > 10) {
            grabbed = true;
        }
        //----------------------------------------------------------------------------------

        // Draw
        //----------------------------------------------------------------------------------
        BeginDrawing();

        ClearBackground(DARKGREEN);

        DrawText("Welcome to Solitaire!", 190, 200, 32, RAYWHITE);

        renderTableau();

        // reserve
        Vector2 card_pos = {1.0 - card_width - TABLEAU_MARGIN, TABLEAU_Y_START-(card_height+TABLEAU_TOP_MARGIN)};
        DrawTextureEx(cardBack, Vector2Multiply(card_pos, screen_dim), 0.0f, card_scale, WHITE);

        renderFoundation();

        // grabbed card
        if (grabbed) {
            Vector2 new_pos = {
                .x = touch_pos.x - card_width/2,
                .y = touch_pos.y - card_height/2
            };
            DrawTextureEx(cardBack, Vector2Multiply(new_pos, screen_dim), 0.0f, card_scale, WHITE);
            pos = new_pos;
        } else {
            DrawTextureEx(cardBack, Vector2Multiply(pos, screen_dim), 0.0f, card_scale, WHITE);
        }

        EndDrawing();
        //----------------------------------------------------------------------------------

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
