/*
 * Neo: a Matrix-themed dodge game for the LilyGO T-Display.
 *
 * Bullets fall from the top of the screen and the player moves left and right
 * to avoid them. Every bullet that passes scores points, the game speeds up
 * over time, and a single hit ends the run.
 */

#include <stdio.h>
#include <stdlib.h>
#include <esp_timer.h>
#include "fonts.h"
#include "graphics.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include "input_output.h"

#ifdef TTGO_S3
#define RIGHT_BUTTON 14
#else
#define RIGHT_BUTTON 35
#endif

#define LEFT_BUTTON 0

// Player hitbox, which is deliberately smaller than the 80x73 px sprite.
#define PLAYER_W 56
#define PLAYER_H 36
#define PLAYER_SPRITE_H 73
#define PLAYER_SPRITE_Y_OFFSET 7
#define PLAYER_BOTTOM_MARGIN 2
#define PLAYER_SPEED 300  // px/s

// Bullet hitbox and pool size.
#define BULLET_W 10
#define BULLET_H 31
#define MAX_BULLETS 20

// Difficulty rises one step every 10 seconds.
#define DIFFICULTY_STEP_US 10000000
#define INITIAL_SPAWN_MS 1500
#define MIN_SPAWN_MS 300
#define SPAWN_STEP_MS 100
#define INITIAL_FALL_SPEED 120  // px/s
#define MAX_FALL_SPEED 600      // no further steps once this is reached
#define FALL_SPEED_STEP 50      // px/s

#define SCORE_PER_DODGE 100
#define MAX_FRAME_TIME_S 0.05f
#define GAME_OVER_DELAY_MS 3000

// Matrix rain on the title screen.
#define RAIN_COLS 40
#define RAIN_SPEED_MIN 30
#define RAIN_SPEED_MAX 80
#define RAIN_LENGTH_MIN 8
#define RAIN_LENGTH_MAX 20
#define RAIN_ROW_HEIGHT 12

static const char RAIN_SYMBOLS[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()";

// Sprites exported from GIMP, defined in neo_main.c and bullet_main.c.
extern image_header neo_main_image;
extern image_header bullet_main_image;

typedef struct {
  float x, y;  // top-left position
  int w, h;    // hitbox size; w == 0 marks an unused bullet slot
  float yvel;  // fall speed in px/s
} entity_t;

typedef struct {
  int col;                     // x position
  int y;                       // y position of the top of the trail
  int speed;                   // fall speed
  int length;                  // trail length in characters
  int chars[RAIN_LENGTH_MAX];  // characters in the trail
} matrix_rain_t;

static float last_rain_time = 0;

// Returns the seconds since the last call, capped so a stall cannot make
// objects jump across the screen.
static float get_frame_time(void) {
  static uint64_t last_time = 0;
  float dt;
  uint64_t current_time = esp_timer_get_time();

  if (last_time == 0) dt = 0;
  else dt = (current_time - last_time) / 1000000.0f;

  if (dt > MAX_FRAME_TIME_S) dt = MAX_FRAME_TIME_S;
  last_time = current_time;
  return dt;
}

// Axis-aligned bounding box collision test.
static int overlap(const entity_t *a, const entity_t *b) {
  return a->x < b->x + b->w && a->x + a->w > b->x &&
         a->y < b->y + b->h && a->y + a->h > b->y;
}

static char random_rain_symbol(void) {
  return RAIN_SYMBOLS[rand() % (sizeof(RAIN_SYMBOLS) - 1)];
}

static int random_rain_speed(void) {
  return rand() % (RAIN_SPEED_MAX - RAIN_SPEED_MIN) + RAIN_SPEED_MIN;
}

static int random_rain_length(void) {
  return rand() % (RAIN_LENGTH_MAX - RAIN_LENGTH_MIN) + RAIN_LENGTH_MIN;
}

// Draws one frame of rain and advances every column.
static void draw_matrix_rain(matrix_rain_t *rain, int bright) {
  uint64_t time = esp_timer_get_time();
  float dt = (time - last_rain_time) / 1000000.0f;
  last_rain_time = time;

  for (int i = 0; i < RAIN_COLS; i++) {
    matrix_rain_t *r = &rain[i];

    for (int j = 0; j < r->length; j++) {
      int draw_y = r->y + j * RAIN_ROW_HEIGHT;
      if (draw_y >= 0 && draw_y < display_height) {
        // The leading character is brightest and the trail fades behind it.
        if (j == r->length - 1) {
          setFontColour(0, bright, bright / 3);
        } else if (j >= r->length - 3) {
          setFontColour(0, bright * 2 / 3, bright / 5);
        } else if (j >= r->length - 6) {
          setFontColour(0, bright / 2, bright / 8);
        } else {
          setFontColour(0, bright / 4, bright / 16);
        }

        char c[2] = {r->chars[j], '\0'};
        print_xy(c, r->col, draw_y);
      }
    }

    r->y += (int)(r->speed * dt * 10);

    // Recycle the column with new properties once it leaves the screen.
    if (r->y > display_height + r->length * RAIN_ROW_HEIGHT) {
      r->y = -(rand() % display_height);
      r->speed = random_rain_speed();
      r->length = random_rain_length();
      for (int j = 0; j < r->length; j++) {
        r->chars[j] = random_rain_symbol();
      }
    }
  }
}

static void init_matrix_rain(matrix_rain_t *rain) {
  last_rain_time = esp_timer_get_time();

  for (int i = 0; i < RAIN_COLS; i++) {
    rain[i].col = (display_width / RAIN_COLS) * i + (display_width / RAIN_COLS / 2);
    rain[i].y = -(rand() % display_height);
    rain[i].speed = random_rain_speed();
    rain[i].length = random_rain_length();

    for (int j = 0; j < rain[i].length; j++) {
      rain[i].chars[j] = random_rain_symbol();
    }
  }
}

static void draw_title_screen(matrix_rain_t *rain) {
  cls(rgbToColour(0, 10, 0));
  draw_matrix_rain(rain, 255);

  setFont(FONT_DEJAVU18);
  setFontColour(0, 255, 70);
  print_xy("THE MATRIX", CENTER, 30);

  setFont(FONT_DEJAVU18);
  setFontColour(0, 200, 50);
  print_xy("SYSTEM FAILURE", CENTER, 65);

  setFont(FONT_UBUNTU16);
  setFontColour(0, 150, 40);
  print_xy("Dodge the bullets.", CENTER, 110);
  print_xy("Survive.", CENTER, 130);

  setFontColour(0, 100, 30);
  print_xy("LEFT:  Move Left", CENTER, 170);
  print_xy("RIGHT: Move Right", CENTER, 190);

  setFont(FONT_SMALL);
  setFontColour(0, 255, 70);
  print_xy("PRESS LEFT TO ENTER", CENTER, 260);

  flip_frame();
}

static void draw_game_over(int score, int high_score) {
  cls(rgbToColour(50, 10, 10));

  setFont(FONT_DEJAVU18);
  setFontColour(255, 100, 100);
  print_xy("SYSTEM CRASH", CENTER, 60);

  setFont(FONT_DEJAVU18);
  setFontColour(255, 255, 255);
  char buf[32];
  sprintf(buf, "Score: %d", score);
  print_xy(buf, CENTER, 110);

  sprintf(buf, "High Score: %d", high_score);
  setFontColour(200, 200, 200);
  print_xy(buf, CENTER, 140);

  setFont(FONT_UBUNTU16);
  setFontColour(100, 200, 100);
  print_xy("GET READY", CENTER, 200);
  print_xy("FOR", CENTER, 220);
  print_xy("THE NEXT BATTLE", CENTER, 240);

  flip_frame();
}

// Activates a bullet slot just above the screen at a random x position.
static void spawn_bullet(entity_t *bullet, int fall_speed) {
  bullet->w = BULLET_W;
  bullet->h = BULLET_H;
  bullet->x = rand() % (display_width - BULLET_W);
  bullet->y = -BULLET_H;
  bullet->yvel = fall_speed;
}

// Runs the title screen and one full game, returning after the player is hit.
static void run_game(void) {
  entity_t *bullets = malloc(sizeof(entity_t) * MAX_BULLETS);
  if (!bullets) return;

  matrix_rain_t *matrix_rain = malloc(sizeof(matrix_rain_t) * RAIN_COLS);
  if (!matrix_rain) {
    free(bullets);
    return;
  }

  set_orientation(PORTRAIT);
  init_matrix_rain(matrix_rain);
  int high_score = storage_read_int("highscore", 0);

  entity_t player;
  player.x = (display_width - PLAYER_W) / 2;
  player.y = (display_height - PLAYER_SPRITE_H) - PLAYER_BOTTOM_MARGIN;
  player.w = PLAYER_W;
  player.h = PLAYER_H;
  player.yvel = 0;

  for (int i = 0; i < MAX_BULLETS; i++) {
    bullets[i].w = 0;
    bullets[i].h = 0;
    bullets[i].yvel = 0;
  }

  int score = 0;
  int spawn_interval_ms = INITIAL_SPAWN_MS;
  int fall_speed = INITIAL_FALL_SPEED;
  uint64_t last_spawn_time = esp_timer_get_time() / 1000;
  uint64_t difficulty_timer = esp_timer_get_time();

  setFont(FONT_UBUNTU16);
  setFontColour(255, 255, 255);

  // Show the title until LEFT is pressed, then wait for it to be released so
  // the same press does not also move the player. Buttons are active low.
  do {
    draw_title_screen(matrix_rain);
  } while (gpio_get_level(LEFT_BUTTON));

  while (!gpio_get_level(LEFT_BUTTON)) {
    vTaskDelay(pdMS_TO_TICKS(20));
  }

  while (1) {
    float dt = get_frame_time();

    cls(rgbToColour(5, 20, 5));

    // Player movement, clamped to the screen edges.
    if (!gpio_get_level(LEFT_BUTTON)) {
      player.x -= PLAYER_SPEED * dt;
      if (player.x < 0) player.x = 0;
    }
    if (!gpio_get_level(RIGHT_BUTTON)) {
      player.x += PLAYER_SPEED * dt;
      if (player.x > display_width - player.w) player.x = display_width - player.w;
    }

    // Spawn into the first free slot. If the pool is full, this spawn is skipped.
    uint64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms - last_spawn_time >= (uint64_t)spawn_interval_ms) {
      for (int i = 0; i < MAX_BULLETS; i++) {
        if (bullets[i].w == 0) {
          spawn_bullet(&bullets[i], fall_speed);
          break;
        }
      }
      last_spawn_time = now_ms;
    }

    // Difficulty ramp. Bullets already in the air speed up too.
    uint64_t now = esp_timer_get_time();
    if (now - difficulty_timer >= DIFFICULTY_STEP_US) {
      difficulty_timer = now;
      if (spawn_interval_ms > MIN_SPAWN_MS) {
        spawn_interval_ms -= SPAWN_STEP_MS;
      }
      if (fall_speed < MAX_FALL_SPEED) {
        fall_speed += FALL_SPEED_STEP;
        for (int i = 0; i < MAX_BULLETS; i++) {
          if (bullets[i].w > 0) {
            bullets[i].yvel = fall_speed;
          }
        }
      }
    }

    // Move bullets. One that leaves the screen frees its slot and scores.
    for (int i = 0; i < MAX_BULLETS; i++) {
      if (bullets[i].w > 0) {
        bullets[i].y += bullets[i].yvel * dt;
        if (bullets[i].y > display_height) {
          bullets[i].w = 0;
          bullets[i].h = 0;
          score += SCORE_PER_DODGE;
          if (score > high_score) {
            high_score = score;
            storage_write_int("highscore", high_score);
          }
        }
      }
    }

    // A single hit ends the run.
    for (int i = 0; i < MAX_BULLETS; i++) {
      if (bullets[i].w > 0 && overlap(&player, &bullets[i])) {
        draw_game_over(score, high_score);
        vTaskDelay(pdMS_TO_TICKS(GAME_OVER_DELAY_MS));
        free(matrix_rain);
        free(bullets);
        return;
      }
    }

    // draw_image positions sprites by their centre.
    draw_image(&neo_main_image, player.x + PLAYER_W / 2,
               player.y + PLAYER_H / 2 + PLAYER_SPRITE_Y_OFFSET);

    for (int i = 0; i < MAX_BULLETS; i++) {
      if (bullets[i].w > 0) {
        draw_image(&bullet_main_image, bullets[i].x + BULLET_W / 2,
                   bullets[i].y + BULLET_H / 2);
      }
    }

    char buf[32];
    sprintf(buf, "Score: %d", score);
    print_xy(buf, 5, 5);
    sprintf(buf, "Hi: %d", high_score);
    print_xy(buf, display_width - 70, 5);

    flip_frame();
  }
}

void app_main(void) {
  input_output_init();
  graphics_init();
  cls(0);

  while (1) {
    run_game();
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
