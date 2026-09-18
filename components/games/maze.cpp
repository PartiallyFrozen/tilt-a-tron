#include "games/maze.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "audio/audio.h"
#include "console/ui.h"
#include "engine/gestures.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"

using namespace wc;

namespace games {

namespace {

const char *TAG = "maze";

constexpr int W = Gfx::W, H = Gfx::H;
constexpr float CXf = 233.0f, CYf = 233.0f;
constexpr float BOARD_R = 200.0f;   // the maze lives inside this circle; HUD sits outside it

// Board look: a wooden labyrinth, like the real toy.
constexpr Color FLOOR = rgb(201, 158, 98);
constexpr Color FLOOR_DARK = rgb(188, 145, 86);
constexpr Color WALL = rgb(104, 68, 34);
constexpr Color WALL_TOP = rgb(150, 104, 58);
constexpr Color WALL_SHADE = rgb(70, 44, 20);
constexpr Color HOLE = rgb(0, 0, 0);
constexpr Color HOLE_RIM = rgb(96, 66, 36);
constexpr Color START = rgb(60, 170, 90);

constexpr float GRAVITY = 1500.0f;     // px/s^2 per g of tilt
constexpr float FRICTION = 1.1f;       // 1/s
constexpr float MAX_SPEED = 430.0f;
constexpr float RESTITUTION = 0.28f;

uint32_t rnd(uint32_t n) { return esp_random() % n; }
float clampf(float v, float a, float b) { return std::max(a, std::min(b, v)); }

struct RectF {
    float x0, y0, x1, y1;
};
struct Hole {
    float x, y;
};
struct IRect {
    int x, y, w, h;
};

enum Phase { READY, PLAYING, FALLING, CLEARED, GAME_OVER };

namespace sfx {
using wc::audio::Tone;
using wc::audio::Wave;

void start() { wc::audio::play({.f0 = 520, .f1 = 780, .ms = 90, .wave = Wave::Triangle, .volume = 0.7f}); }

void thunk(float strength)
{
    wc::audio::play(
        {.f0 = 150, .f1 = 80, .ms = 45, .wave = Wave::Triangle, .volume = clampf(0.25f + strength * 0.7f, 0.25f, 0.95f)});
}

void roll(float speed01)
{
    wc::audio::play({.f0 = 90, .f1 = 70, .ms = 28, .wave = Wave::Noise, .volume = 0.05f + 0.10f * speed01});
}

void fall()
{
    const Tone t[] = {{.f0 = 640, .f1 = 70, .ms = 380, .wave = Wave::Square, .volume = 0.7f},
                      {.f0 = 110, .f1 = 50, .ms = 130, .wave = Wave::Noise, .volume = 0.5f, .delay_ms = 360}};
    wc::audio::play(t, 2);
}

void cleared()
{
    const Tone t[] = {{.f0 = 523, .ms = 90, .wave = Wave::Square, .volume = 0.7f, .delay_ms = 0},
                      {.f0 = 659, .ms = 90, .wave = Wave::Square, .volume = 0.7f, .delay_ms = 90},
                      {.f0 = 784, .ms = 90, .wave = Wave::Square, .volume = 0.7f, .delay_ms = 180},
                      {.f0 = 1047, .ms = 220, .wave = Wave::Square, .volume = 0.8f, .delay_ms = 270}};
    wc::audio::play(t, 4);
}

void gameOver()
{
    const Tone t[] = {{.f0 = 392, .ms = 160, .wave = Wave::Square, .volume = 0.7f, .delay_ms = 0},
                      {.f0 = 330, .ms = 160, .wave = Wave::Square, .volume = 0.7f, .delay_ms = 160},
                      {.f0 = 262, .ms = 320, .wave = Wave::Square, .volume = 0.7f, .delay_ms = 320}};
    wc::audio::play(t, 3);
}
}  // namespace sfx

}  // namespace

struct Maze::State {
    // ---- run
    int level = 1, lives = 3, best = 1;
    Phase phase = READY;
    float phase_t = 0;

    // ---- maze
    int n = 7;             // grid is n x n, clipped to the round board
    float cs = 56;         // cell size in pixels
    float ox = 0, oy = 0;  // top-left of the grid
    float wall_t = 7;      // wall thickness
    std::vector<uint8_t> valid;   // cell is on the board
    std::vector<uint8_t> open_e;  // passage to the cell on the right
    std::vector<uint8_t> open_s;  // passage to the cell below
    std::vector<RectF> walls;
    std::vector<Hole> holes;
    float hole_r = 12;
    int start_cell = 0, finish_cell = 0;

    // ---- ball
    float bx = 0, by = 0, vx = 0, vy = 0, br = 12;
    float tilt_nx = 0, tilt_ny = 0;   // "level": how the watch was held at the tap
    float fall_x = 0, fall_y = 0;
    float roll_t = 0;
    float still_t = 0;                // seconds since the ball last really moved

    // ---- drawing
    Color *board = nullptr;           // the static scene, to restore under the ball
    bool redraw = true;
    IRect ball_rect{0, 0, 0, 0};
    int drawn_lives = -1;
    Phase drawn_phase = GAME_OVER;

    // ---- menu / input
    Gestures ges;
    bool menu = false, menu_dirty = false;

    // ------------------------------------------------------------------ helpers
    int idx(int c, int r) const { return r * n + c; }
    float cellX(int i) const { return ox + (i % n + 0.5f) * cs; }
    float cellY(int i) const { return oy + (i / n + 0.5f) * cs; }

    bool isOpen(int a, int b) const
    {
        if (b == a + 1) return open_e[a];
        if (b == a - 1) return open_e[b];
        if (b == a + n) return open_s[a];
        if (b == a - n) return open_s[b];
        return false;
    }

    void neighbours(int i, int out[4], int &count) const
    {
        count = 0;
        const int c = i % n, r = i / n;
        if (c + 1 < n && valid[i + 1]) out[count++] = i + 1;
        if (c > 0 && valid[i - 1]) out[count++] = i - 1;
        if (r + 1 < n && valid[i + n]) out[count++] = i + n;
        if (r > 0 && valid[i - n]) out[count++] = i - n;
    }

    // Farthest cell from `from` through the passages (breadth-first).
    int farthest(int from, std::vector<int> *dist_out = nullptr) const
    {
        std::vector<int> dist(n * n, -1), queue;
        dist[from] = 0;
        queue.push_back(from);
        int last = from;
        for (size_t q = 0; q < queue.size(); q++) {
            const int i = queue[q];
            last = i;
            int nb[4], cnt;
            neighbours(i, nb, cnt);
            for (int k = 0; k < cnt; k++) {
                if (dist[nb[k]] >= 0 || !isOpen(i, nb[k])) continue;
                dist[nb[k]] = dist[i] + 1;
                queue.push_back(nb[k]);
            }
        }
        if (dist_out) *dist_out = dist;
        return last;
    }

    // ------------------------------------------------------------------ generation
    void generate()
    {
        // Bigger, tighter mazes as the levels climb.
        n = std::min(7 + (level - 1) / 2 * 2, 13);
        cs = std::floor(2 * BOARD_R / n);
        ox = CXf - n * cs / 2;
        oy = CYf - n * cs / 2;
        wall_t = std::max(4.0f, std::floor(cs / 8));
        br = cs * 0.26f;
        hole_r = cs * 0.30f;

        valid.assign(n * n, 0);
        open_e.assign(n * n, 0);
        open_s.assign(n * n, 0);
        int first = -1;
        for (int r = 0; r < n; r++) {
            for (int c = 0; c < n; c++) {
                const float dx = ox + (c + 0.5f) * cs - CXf, dy = oy + (r + 0.5f) * cs - CYf;
                if (std::sqrt(dx * dx + dy * dy) <= BOARD_R - cs * 0.30f) {
                    valid[idx(c, r)] = 1;
                    if (first < 0) first = idx(c, r);
                }
            }
        }

        // Depth-first carve: a perfect maze (exactly one route between any two cells).
        std::vector<uint8_t> seen(n * n, 0);
        std::vector<int> stack{first};
        seen[first] = 1;
        while (!stack.empty()) {
            const int i = stack.back();
            int nb[4], cnt;
            neighbours(i, nb, cnt);
            int fresh[4], fc = 0;
            for (int k = 0; k < cnt; k++)
                if (!seen[nb[k]]) fresh[fc++] = nb[k];
            if (fc == 0) {
                stack.pop_back();
                continue;
            }
            const int j = fresh[rnd(fc)];
            if (j == i + 1) open_e[i] = 1;
            else if (j == i - 1) open_e[j] = 1;
            else if (j == i + n) open_s[i] = 1;
            else open_s[j] = 1;
            seen[j] = 1;
            stack.push_back(j);
        }

        // Start and finish at the two ends of the longest route through the maze.
        start_cell = farthest(first);
        finish_cell = farthest(start_cell);

        // Holes wait at the end of wrong turns. More of them as levels climb.
        holes.clear();
        const float chance = std::min(0.45f + 0.07f * level, 0.9f);
        for (int i = 0; i < n * n; i++) {
            if (!valid[i] || i == start_cell || i == finish_cell) continue;
            int nb[4], cnt, exits = 0;
            neighbours(i, nb, cnt);
            for (int k = 0; k < cnt; k++) exits += isOpen(i, nb[k]);
            if (exits == 1 && rnd(1000) < uint32_t(chance * 1000)) holes.push_back({cellX(i), cellY(i)});
        }

        buildWalls();
        ESP_LOGI(TAG, "level %d: %dx%d, %u walls, %u holes", level, n, n, unsigned(walls.size()), unsigned(holes.size()));
    }

    void addWall(float x0, float y0, float x1, float y1) { walls.push_back({x0, y0, x1, y1}); }

    void buildWalls()
    {
        walls.clear();
        const float h = wall_t / 2;
        for (int r = 0; r < n; r++) {
            for (int c = 0; c < n; c++) {
                const int i = idx(c, r);
                if (!valid[i]) continue;
                const float x = ox + c * cs, y = oy + r * cs;
                // Top and left edges: a wall unless there's a passage to that neighbour.
                const bool up = r > 0 && valid[i - n] && open_s[i - n];
                const bool left = c > 0 && valid[i - 1] && open_e[i - 1];
                if (!up) addWall(x - h, y - h, x + cs + h, y + h);
                if (!left) addWall(x - h, y - h, x + h, y + cs + h);
                // Bottom and right only where the board ends (otherwise the neighbour draws it).
                const bool has_down = r + 1 < n && valid[i + n];
                const bool has_right = c + 1 < n && valid[i + 1];
                if (!has_down) addWall(x - h, y + cs - h, x + cs + h, y + cs + h);
                if (!has_right) addWall(x + cs - h, y - h, x + cs + h, y + cs + h);
            }
        }
    }

    // ------------------------------------------------------------------ flow
    void loadBest()
    {
        nvs_handle_t h;
        if (nvs_open("maze", NVS_READONLY, &h) != ESP_OK) return;
        uint8_t v;
        if (nvs_get_u8(h, "best", &v) == ESP_OK && v > 0) best = v;
        nvs_close(h);
    }

    void saveBest()
    {
        nvs_handle_t h;
        if (nvs_open("maze", NVS_READWRITE, &h) != ESP_OK) return;
        nvs_set_u8(h, "best", uint8_t(std::min(best, 250)));
        nvs_commit(h);
        nvs_close(h);
    }

    void placeBall()
    {
        bx = cellX(start_cell);
        by = cellY(start_cell);
        vx = vy = 0;
    }

    void newLevel()
    {
        generate();
        placeBall();
        phase = READY;
        phase_t = 0;
        redraw = true;
    }

    void newGame()
    {
        level = 1;
        lives = 3;
        newLevel();
    }

    void beginPlay(const InputState &in)
    {
        // However the watch is being held right now counts as level.
        tilt_nx = in.tilt.ax;
        tilt_ny = in.tilt.ay;
        phase = PLAYING;
        phase_t = 0;
        still_t = 0;
        redraw = true;
        sfx::start();
    }

    // ------------------------------------------------------------------ physics
    bool collide()
    {
        bool hit = false;
        for (const RectF &w : walls) {
            const float px = clampf(bx, w.x0, w.x1), py = clampf(by, w.y0, w.y1);
            float dx = bx - px, dy = by - py;
            const float d2 = dx * dx + dy * dy;
            if (d2 >= br * br) continue;
            float d = std::sqrt(d2);
            if (d < 0.001f) {   // center inside the wall: push out along the way we came
                dx = -vx;
                dy = -vy;
                d = std::max(0.001f, std::sqrt(dx * dx + dy * dy));
            }
            const float nx = dx / d, ny = dy / d;
            bx = px + nx * br;
            by = py + ny * br;
            const float vn = vx * nx + vy * ny;
            if (vn < 0) {
                vx -= (1 + RESTITUTION) * vn * nx;
                vy -= (1 + RESTITUTION) * vn * ny;
                if (-vn > 70) sfx::thunk(clampf(-vn / MAX_SPEED, 0, 1));
            }
            hit = true;
        }
        return hit;
    }

    void stepBall(const InputState &in, float dt)
    {
        const float ax = (in.tilt.ax - tilt_nx) * GRAVITY, ay = (in.tilt.ay - tilt_ny) * GRAVITY;
        vx += ax * dt;
        vy += ay * dt;

        // The floor dips toward a hole: get close and it starts to pull you in.
        for (const Hole &h : holes) {
            const float dx = h.x - bx, dy = h.y - by, d = std::sqrt(dx * dx + dy * dy);
            if (d < hole_r * 1.5f && d > 0.5f) {
                const float pull = 900.0f * (1.0f - d / (hole_r * 1.5f));
                vx += dx / d * pull * dt;
                vy += dy / d * pull * dt;
            }
        }

        const float damp = std::max(0.0f, 1.0f - FRICTION * dt);
        vx *= damp;
        vy *= damp;
        float speed = std::sqrt(vx * vx + vy * vy);
        if (speed > MAX_SPEED) {
            vx *= MAX_SPEED / speed;
            vy *= MAX_SPEED / speed;
            speed = MAX_SPEED;
        }

        // Small steps so a fast ball can't tunnel through a thin wall.
        const int steps = std::max(1, int(std::ceil(speed * dt / (br * 0.45f))));
        for (int s = 0; s < steps; s++) {
            bx += vx * dt / steps;
            by += vy * dt / steps;
            collide();
        }

        // Rolling rumble, faster ticks the faster it goes.
        still_t = speed > 12 ? 0 : still_t + dt;
        roll_t -= dt;
        if (speed > 45 && roll_t <= 0) {
            sfx::roll(clampf(speed / MAX_SPEED, 0, 1));
            roll_t = 0.16f - 0.10f * clampf(speed / MAX_SPEED, 0, 1);
        }

        for (const Hole &h : holes) {
            const float dx = h.x - bx, dy = h.y - by;
            if (dx * dx + dy * dy < hole_r * hole_r * 0.55f) {
                fall_x = h.x;
                fall_y = h.y;
                phase = FALLING;
                phase_t = 0;
                lives--;
                sfx::fall();
                return;
            }
        }

        const float fx = cellX(finish_cell) - bx, fy = cellY(finish_cell) - by;
        if (fx * fx + fy * fy < (cs * 0.30f) * (cs * 0.30f)) {
            phase = CLEARED;
            phase_t = 0;
            redraw = true;
            sfx::cleared();
            if (level + 1 > best) {
                best = level + 1;
                saveBest();
            }
        }
    }

    void update(Engine &e, float dt)
    {
        const InputState &in = e.input();
        ges.update(in.touch);
        phase_t += dt;

        if (menu) {
            if (ges.tap) menuTap(e, ges.x, ges.y);
            if (ges.swipe_right || (in.clicked & BTN_B)) closeMenu();
            return;
        }
        if (ges.swipe_left && (phase == PLAYING || phase == READY)) {
            menu = true;
            menu_dirty = true;
            return;
        }

        switch (phase) {
        case READY:
            if (ges.tap) beginPlay(in);
            break;
        case PLAYING:
            stepBall(in, dt);
            break;
        case FALLING:
            // The ball slides to the middle of the hole and drops away.
            bx += (fall_x - bx) * std::min(1.0f, dt * 14);
            by += (fall_y - by) * std::min(1.0f, dt * 14);
            if (phase_t > 0.9f) {
                if (lives <= 0) {
                    phase = GAME_OVER;
                    phase_t = 0;
                    redraw = true;
                    sfx::gameOver();
                } else {
                    placeBall();
                    phase = READY;
                    phase_t = 0;
                    redraw = true;
                }
            }
            break;
        case CLEARED:
            if (phase_t > 1.4f) {
                level++;
                newLevel();
            }
            break;
        case GAME_OVER:
            if (ges.tap && phase_t > 0.6f) newGame();
            break;
        }
    }

    // ------------------------------------------------------------------ menu
    void closeMenu()
    {
        menu = false;
        redraw = true;
    }

    void menuTap(Engine &e, int x, int y)
    {
        namespace ui = console::ui;
        if (ui::rowRect(0).hit(x, y)) {
            // Hold the watch how you like, then tap: that becomes level.
            tilt_nx = e.input().tilt.ax;
            tilt_ny = e.input().tilt.ay;
            vx = vy = 0;
            closeMenu();
        } else if (ui::rowRect(1).hit(x, y)) {
            wc::audio::setVolume(wc::audio::volume() == 0 ? 2 : 0);
            if (wc::audio::volume() > 0) sfx::start();
            menu_dirty = true;
        } else if (ui::rowRect(2).hit(x, y)) {
            newGame();   // back to level 1 with three balls
            closeMenu();
        } else if (ui::buttonRect(0, 2).hit(x, y)) {
            closeMenu();
        } else if (ui::buttonRect(1, 2).hit(x, y)) {
            closeMenu();
            e.goHome();
        }
    }

    void drawMenu(Gfx &g)
    {
        namespace ui = console::ui;
        ui::clearScreen(g);
        ui::title(g, "PAUSED");
        ui::row(g, 0, "RECENTER TILT", "SET", ui::ACCENT);
        ui::row(g, 1, "SOUND", wc::audio::volume() ? "ON" : "OFF", wc::audio::volume() ? ui::GO : ui::DIM);
        ui::row(g, 2, "NEW GAME", "GO", ui::ACCENT);
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", best);
        ui::row(g, 3, "BEST LEVEL", buf, ui::LABEL);
        ui::button(g, ui::buttonRect(0, 2), "RESUME");
        ui::outlineButton(g, ui::buttonRect(1, 2), "HOME");
    }

    // ------------------------------------------------------------------ drawing
    void fillDisc(Gfx &g, float cx, float cy, float r, Color c)
    {
        const int y0 = int(std::floor(cy - r)), y1 = int(std::ceil(cy + r));
        for (int y = y0; y <= y1; y++) {
            const float dy = y + 0.5f - cy;
            if (dy * dy > r * r) continue;
            const float half = std::sqrt(r * r - dy * dy);
            const int xa = int(std::ceil(cx - half - 0.5f)), xb = int(std::floor(cx + half - 0.5f));
            if (xb >= xa) g.fillRect(xa, y, xb - xa + 1, 1, c);
        }
    }

    void drawBoard(Gfx &g)
    {
        g.clear(colors::black);

        // Floor, with a faint plank pattern so it reads as wood.
        for (int i = 0; i < n * n; i++) {
            if (!valid[i]) continue;
            const int x = int(ox + (i % n) * cs), y = int(oy + (i / n) * cs);
            g.fillRect(x, y, int(cs) + 1, int(cs) + 1, ((i % n) + (i / n)) % 2 ? FLOOR : FLOOR_DARK);
        }

        // Finish: a checkered flag square. Start: a green ring.
        {
            const float fx = cellX(finish_cell), fy = cellY(finish_cell), half = cs * 0.32f;
            const int q = std::max(3, int(half * 2 / 4));
            for (int r = 0; r < 4; r++)
                for (int c = 0; c < 4; c++)
                    g.fillRect(int(fx - half) + c * q, int(fy - half) + r * q, q, q,
                               (r + c) % 2 ? colors::white : colors::black);
            fillDisc(g, cellX(start_cell), cellY(start_cell), cs * 0.34f, START);
            fillDisc(g, cellX(start_cell), cellY(start_cell), cs * 0.24f, ((start_cell % n) + (start_cell / n)) % 2 ? FLOOR : FLOOR_DARK);
        }

        for (const Hole &h : holes) {
            fillDisc(g, h.x, h.y, hole_r + 2, HOLE_RIM);
            fillDisc(g, h.x, h.y, hole_r, HOLE);
        }

        // Walls with a lit top edge and a shadow underneath, for a little depth.
        for (const RectF &w : walls)
            g.fillRect(int(w.x0) + 2, int(w.y0) + 3, int(w.x1 - w.x0), int(w.y1 - w.y0), WALL_SHADE);
        for (const RectF &w : walls) {
            const int x = int(w.x0), y = int(w.y0), ww = int(w.x1 - w.x0), hh = int(w.y1 - w.y0);
            g.fillRect(x, y, ww, hh, WALL);
            g.fillRect(x, y, ww, 1, WALL_TOP);
            g.fillRect(x, y, 1, hh, WALL_TOP);
        }

        // HUD lives outside the board: level at the top, best at the bottom.
        char buf[24];
        snprintf(buf, sizeof(buf), "LEVEL %d", level);
        g.textCentered(Gfx::CX, 17, buf, colors::white, 2, true);
        snprintf(buf, sizeof(buf), "BEST %d", best);
        g.textCentered(Gfx::CX, 450, buf, console::ui::DIM, 2, true);

        std::memcpy(board, g.pixels(), W * H * sizeof(Color));
        ball_rect = {0, 0, 0, 0};
        drawn_lives = -1;
    }

    void restore(Gfx &g, const IRect &r)
    {
        if (r.w <= 0 || r.h <= 0) return;
        g.setClip(r.x, r.y, r.w, r.h);
        g.blit(board, W, H, 0, 0);
        g.clearClip();
    }

    void drawLives(Gfx &g)
    {
        // Spare balls, tucked beside the level label.
        restore(g, {Gfx::CX + 62, 8, 60, 20});
        for (int i = 0; i < lives; i++) fillDisc(g, Gfx::CX + 72 + i * 17, 17, 6, rgb(210, 214, 222));
        drawn_lives = lives;
    }

    void drawBall(Gfx &g, float scale)
    {
        const float r = br * scale;
        if (r < 1) {
            ball_rect = {0, 0, 0, 0};
            return;
        }
        fillDisc(g, bx + 2, by + 3, r, rgb(90, 62, 30));          // shadow on the wood
        fillDisc(g, bx, by, r, rgb(168, 174, 184));                // steel
        fillDisc(g, bx - r * 0.12f, by - r * 0.12f, r * 0.78f, rgb(206, 211, 219));
        fillDisc(g, bx - r * 0.34f, by - r * 0.36f, r * 0.30f, colors::white);
        const int pad = int(r) + 5;
        ball_rect = {int(bx) - pad, int(by) - pad, 2 * pad + 4, 2 * pad + 5};
    }

    void banner(Gfx &g, const char *top, const char *bottom, Color c)
    {
        namespace ui = console::ui;
        g.fillRect(83, 196, 300, bottom ? 78 : 50, ui::PANEL);
        g.rect(83, 196, 300, bottom ? 78 : 50, ui::BOX);
        g.textCentered(Gfx::CX, 221, top, c, 3, true);
        if (bottom) g.textCentered(Gfx::CX, 254, bottom, ui::LABEL, 2, true);
    }

    void draw(Gfx &g)
    {
        if (menu) {
            if (menu_dirty) {
                drawMenu(g);
                menu_dirty = false;
            }
            return;
        }

        if (redraw || phase != drawn_phase) {
            drawBoard(g);
            redraw = false;
            drawn_phase = phase;
            drawLives(g);
            drawBall(g, 1.0f);
            char buf[32];
            switch (phase) {
            case READY: banner(g, "TAP TO START", "TILT TO ROLL", colors::white); break;
            case CLEARED: banner(g, "LEVEL CLEAR!", nullptr, console::ui::GO); break;
            case GAME_OVER:
                snprintf(buf, sizeof(buf), "REACHED LEVEL %d", level);
                banner(g, "GAME OVER", buf, console::ui::DANGER);
                break;
            default: break;
            }
            return;
        }

        if (phase == PLAYING || phase == FALLING) {
            restore(g, ball_rect);
            drawBall(g, phase == FALLING ? std::max(0.0f, 1.0f - phase_t / 0.45f) : 1.0f);
        }
        if (lives != drawn_lives) drawLives(g);
    }
};

Maze::Maze() : s_(new State) {}
Maze::~Maze() { delete s_; }

void Maze::begin(Engine &e)
{
    s_->board = static_cast<Color *>(heap_caps_malloc(W * H * sizeof(Color), MALLOC_CAP_SPIRAM));
    if (!s_->board) ESP_LOGE(TAG, "no memory for the board");
    s_->loadBest();
    s_->newGame();
}

void Maze::enter(Engine &e)
{
    s_->redraw = true;
    // Coming back from the home screen mid-roll: pause rather than let it run blind.
    if (s_->phase == PLAYING) {
        s_->menu = true;
        s_->menu_dirty = true;
    }
}

bool Maze::keepAwake() const
{
    // A marble game has no button presses, so count a rolling ball as "in use",
    // but not a watch left on the table with the ball at rest.
    return !s_->menu && s_->phase == PLAYING && s_->still_t < 20.0f;
}

void Maze::update(Engine &e, float dt)
{
    if (!s_->board) return;
    s_->update(e, std::min(dt, 1.0f / 30));
}

void Maze::draw(Engine &e, Gfx &g)
{
    if (!s_->board) return;
    s_->draw(g);
}

}  // namespace games
