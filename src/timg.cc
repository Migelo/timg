// -*- mode: c++; c-basic-offset: 4; indent-tabs-mode: nil; -*-
// (c) 2016 Henner Zeller <h.zeller@acm.org>
//
// timg - a terminal image viewer.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation version 2.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://gnu.org/licenses/gpl-2.0.txt>
//
// To compile this image viewer, first get image-magick development files
// $ sudo apt-get install libgraphicsmagick++-dev

#include "terminal-canvas.h"
#include "timg-time.h"

#include <assert.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

#include <Magick++.h>

#include <vector>

#define TIMG_VERSION "0.9.9"

using timg::Duration;
using timg::Time;

volatile sig_atomic_t interrupt_received = 0;
static void InterruptHandler(int signo) {
  interrupt_received = 1;
}

void CopyToFramebuffer(const Magick::Image &img, timg::Framebuffer *result) {
    assert(result->width() >= (int) img.columns()
           && result->height() >= (int) img.rows());
    for (size_t y = 0; y < img.rows(); ++y) {
        for (size_t x = 0; x < img.columns(); ++x) {
            const Magick::Color &c = img.pixelColor(x, y);
            if (c.alphaQuantum() >= 255)
                continue;
            result->SetPixel(x, y,
                             ScaleQuantumToChar(c.redQuantum()),
                             ScaleQuantumToChar(c.greenQuantum()),
                             ScaleQuantumToChar(c.blueQuantum()));
        }
    }
}

// Frame already prepared as the buffer to be sent, so copy to terminal-buffer
// does not have to be done online. Also knows about the animation delay.
class PreprocessedFrame {
public:
    PreprocessedFrame(const Magick::Image &img)
        : delay_(DurationFromImgDelay(img)),
          framebuffer_(img.columns(), img.rows()) {
        CopyToFramebuffer(img, &framebuffer_);
    }
    Duration delay() const { return delay_; }
    const timg::Framebuffer &framebuffer() const { return framebuffer_; }

private:
    static Duration DurationFromImgDelay(const Magick::Image &img) {
        int delay_time = img.animationDelay();  // in 1/100s of a second.
        if (delay_time < 1) delay_time = 10;
        return Duration::Millis(delay_time * 10);
    }
    const Duration delay_;
    timg::Framebuffer framebuffer_;
};

static void RenderBackground(int width, int height,
                             const char *bg, const char *pattern,
                             Magick::Image *bgimage) {
    *bgimage = Magick::Image(Magick::Geometry(width, height),
                             Magick::Color(bg ? bg : "black"));
    if (pattern && strlen(pattern)) {
        bgimage->fillColor(pattern);
        for (int x = 0; x < width; x++) {
            for (int y = 0; y < height; y++) {
                if ((x + y) % 2 == 0) {
                    bgimage->draw(Magick::DrawablePoint(x, y));
                }
            }
        }
    }
}

static bool EndsWith(const char *filename, const char *suffix) {
    size_t flen = strlen(filename);
    size_t slen = strlen(suffix);
    if (flen < slen) return false;
    return strcasecmp(filename + flen - slen, suffix) == 0;
}

struct ScaleOptions {
    // If image is smaller than screen, only upscale if do_upscale is set.
    bool do_upscale;
    bool fill_width;   // Fill screen width, allow overflow height.
    bool fill_height;  // Fill screen height, allow overflow width.
};

// Returns 'true' if anything is to do to the picture.
static bool ScaleWithOptions(int img_width, int img_height,
                             const int screen_width, const int screen_height,
                             const ScaleOptions &options,
                             int *target_width, int *target_height) {
    const float width_fraction = (float)screen_width / img_width;
    const float height_fraction = (float)screen_height / img_height;

    // If the image < screen, only upscale if do_upscale requested
    if (!options.do_upscale &&
        (options.fill_height || width_fraction > 1.0) &&
        (options.fill_width || height_fraction > 1.0)) {
        *target_width = img_width;
        *target_height = img_height;
        return false;
    }

    *target_width = screen_width;
    *target_height = screen_height;

    if (options.fill_width && options.fill_height) {
        // Fill as much as we can get in available space.
        // Largest scale fraction determines that.
        const float larger_fraction = (width_fraction > height_fraction)
            ? width_fraction
            : height_fraction;
        *target_width = (int) roundf(larger_fraction * img_width);
        *target_height = (int) roundf(larger_fraction * img_height);
    }
    else if (options.fill_height) {
        // Make things fit in vertical space.
        // While the height constraint stays the same, we can expand width to
        // wider than screen.
        *target_width = (int) roundf(height_fraction * img_width);
    }
    else if (options.fill_width) {
        // dito, vertical. Make things fit in horizontal, overflow vertical.
        *target_height = (int) roundf(width_fraction * img_height);
    }

    return true;
}

// Load still image or animation. If this is a multi-image container, try
// to guess if this is an animation or just a sequence of images.
// Scale, to fit "screen_width" and "screen_height"; store in "result".
static bool LoadImageAndScale(const char *filename,
                              int screen_width, int screen_height,
                              const ScaleOptions &scale_options,
                              bool do_antialias,
                              const char *bg_color, const char *pattern_color,
                              std::vector<Magick::Image> *result,
                              bool *is_animation) {
    std::vector<Magick::Image> frames;
    try {
        readImages(&frames, filename);
    } catch (std::exception& e) {
        fprintf(stderr, "Trouble loading %s (%s)\n", filename, e.what());
        return false;
    }
    if (frames.size() == 0) {
        fprintf(stderr, "No image found.");
        return false;
    }

    // We don't really know if something is an animation from the frames we
    // got back (or is there ?), so we use a blacklist approach here: filenames
    // that are known to be containers for multiple independent images are
    // considered not an animation.
    const bool could_be_animation =
        !EndsWith(filename, "ico") && !EndsWith(filename, "pdf");
    // Put together the animation from single frames. GIFs can have nasty
    // disposal modes, but they are handled nicely by coalesceImages()
    if (frames.size() > 1 && could_be_animation) {
        Magick::coalesceImages(result, frames.begin(), frames.end());
        *is_animation = true;
    } else {
        result->insert(result->end(), frames.begin(), frames.end());
        *is_animation = false;
    }

    for (size_t i = 0; i < result->size(); ++i) {
        Magick::Image *const img = &(*result)[i];
        int target_width = 0, target_height = 0;
        if (ScaleWithOptions(img->columns(), img->rows(),
                             screen_width, screen_height,
                             scale_options,
                             &target_width, &target_height)) {
            if (do_antialias)
                img->scale(Magick::Geometry(target_width, target_height));
            else
                img->sample(Magick::Geometry(target_width, target_height));
        }

        if (bg_color || pattern_color) {
            Magick::Image target;
            try {
                RenderBackground(img->columns(), img->rows(),
                                 bg_color, pattern_color, &target);
            } catch (std::exception& e) {
                fprintf(stderr, "Trouble rendering background (%s)\n",
                        e.what());
                return false;
            }
            target.composite(*img, 0, 0, Magick::OverCompositeOp);
            target.animationDelay(img->animationDelay());
            *img = target;
        }
    }

    return true;
}

void DisplayImageSequence(const std::vector<Magick::Image> &image_sequence,
                          bool is_animation,
                          Duration duration, int max_frames, int loops,
                          timg::TerminalCanvas *canvas) {
    if (max_frames == -1) {
        max_frames = image_sequence.size();
    } else {
        max_frames = std::min(max_frames, (int)image_sequence.size());
    }

    // Convert to preprocessed frames.
    std::vector<PreprocessedFrame*> frames;
    for (int i = 0; i < max_frames; ++i) {
        frames.emplace_back(new PreprocessedFrame(image_sequence[i]));
    }

    const Time end_time = Time::Now() + duration;
    int last_height = -1;  // First one will not have a height.
    if (frames.size() == 1 || !is_animation)
        loops = 1;   // If there is no animation, nothing to repeat.
    for (int k = 0;
         (loops < 0 || k < loops)
             && !interrupt_received
             && Time::Now() < end_time;
         ++k) {
        for (const auto &frame : frames) {
            const Time frame_start = Time::Now();
            if (interrupt_received || frame_start >= end_time)
                break;
            if (is_animation && last_height > 0) {
                canvas->JumpUpPixels(last_height);
            }
            canvas->Send(frame->framebuffer());
            last_height = frame->framebuffer().height();
            const Time frame_finish = frame_start + frame->delay();
            frame_finish.WaitUntil();
        }
    }
}

static int gcd(int a, int b) { return b == 0 ? a : gcd(b, a % b); }

void DisplayScrolling(const Magick::Image &img, Duration scroll_delay,
                      Duration duration, int loops,
                      const int display_w, const int display_h,
                      int dx, int dy,
                      timg::TerminalCanvas *canvas) {
    const int img_width = img.columns();
    const int img_height = img.rows();

    // Since the user can choose the number of cycles we go through it,
    // we need to calculate what the minimum number of steps is we need
    // to do the scroll. If this is just in one direction, that is simple: the
    // number of pixel in that direction. If we go diagonal, then it is
    // essentially the least common multiple of steps.
    const int x_steps = (dx == 0)
        ? 1
        : ((img_width % abs(dx) == 0) ? img_width / abs(dx) : img_width);
    const int y_steps = (dy == 0)
        ? 1
        : ((img_height % abs(dy) == 0) ? img_height / abs(dy) : img_height);
    const int64_t cycle_steps =  x_steps * y_steps / gcd(x_steps, y_steps);

    // Depending if we go forward or backward, we want to start out aligned
    // right or left.
    // For negative direction, guarantee that we never run into negative numbers.
    const int64_t x_init = (dx < 0)
        ? (img_width - display_w - dx*cycle_steps) : 0;
    const int64_t y_init = (dy < 0)
        ? (img_height - display_h - dy*cycle_steps) : 0;
    bool is_first = true;

    // Accessing the original image in a loop is very slow with ImageMagic, so
    // we preprocess this first and create our own fast copy.
    timg::Framebuffer preprocessed(img.columns(), img.rows());
    CopyToFramebuffer(img, &preprocessed);

    timg::Framebuffer display_fb(display_w, display_h);
    const Time end_time = Time::Now() + duration;
    for (int k = 0;
         (loops < 0 || k < loops)
             && !interrupt_received
             && Time::Now() < end_time;
         ++k) {
        for (int64_t cycle_pos = 0; cycle_pos <= cycle_steps; ++cycle_pos) {
            const Time frame_start = Time::Now();
            if (interrupt_received || frame_start >= end_time)
                break;
            const int64_t x_cycle_pos = dx*cycle_pos;
            const int64_t y_cycle_pos = dy*cycle_pos;
            for (int y = 0; y < display_h; ++y) {
                for (int x = 0; x < display_w; ++x) {
                    const int x_src = (x_init + x_cycle_pos + x) % img_width;
                    const int y_src = (y_init + y_cycle_pos + y) % img_height;
                    display_fb.SetPixel(x, y, preprocessed.at(x_src, y_src));
                }
            }
            if (!is_first) {
                canvas->JumpUpPixels(display_fb.height());
            }
            canvas->Send(display_fb);
            is_first = false;
            const Time frame_finish = frame_start + scroll_delay;
            frame_finish.WaitUntil();
        }
    }
}


static int usage(const char *progname, int w, int h) {
    fprintf(stderr, "usage: %s [options] <image> [<image>...]\n", progname);
    fprintf(stderr, "Options:\n"
            "\t-g<w>x<h>  : Output pixel geometry. Default from terminal %dx%d\n"
            "\t-w<seconds>: If multiple images given: Wait time between (default: 0.0).\n"
            "\t-a         : Switch off antialiasing (default: on)\n"
            "\t-W         : Scale to fit width of terminal (default: "
            "fit terminal width and height)\n"
            "\t-U         : Toggle Upscale. If an image is smaller than\n"
            "\t             the terminal size, scale it up to full size.\n"
            "\t-b<str>    : Background color to use on transparent images (default '').\n"
            "\t-B<str>    : Checkerboard pattern color to use on transparent images (default '').\n"
            "\t-C         : Clear screen before showing images.\n"
            "\t-F         : Print filename before showing images.\n"
            "\t-E         : Don't hide the cursor while showing images.\n"
            "\t-v         : Print version and exit.\n"

            "\n  Scrolling\n"
            "\t-s[<ms>]   : Scroll horizontally (optionally: delay ms (60)).\n"
            "\t-d<dx:dy>  : delta x and delta y when scrolling (default: 1:0).\n"

            "\n  For Animations and Scrolling\n"
            "\t-t<seconds>: Stop after this time.\n"
            "\t-c<num>    : Number of runs through a full cycle.\n"
            "\t-f<num>    : Only animation: number of frames to render.\n"

            "\nIf both -c and -t are given, whatever comes first stops.\n"
            "If both -w and -t are given for some animation/scroll, -t "
            "takes precedence\n",
            w, h);
    return 1;
}

int main(int argc, char *argv[]) {
    Magick::InitializeMagick(*argv);

    struct winsize w = {};
    const bool winsize_success = (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0);
    const int term_width = w.ws_col;
    const int term_height = 2 * (w.ws_row-1);  // double number of pixels high.

    ScaleOptions scale_options;
    scale_options.do_upscale = false;
    scale_options.fill_width = false;
    scale_options.fill_height = false;

    bool do_scroll = false;
    bool do_clear = false;
    bool do_antialias = true;
    bool show_filename = false;
    bool hide_cursor = true;
    int width = term_width;
    int height = term_height;
    int max_frames = -1;
    const char *bg_color = nullptr;
    const char *pattern_color = nullptr;
    Duration duration = Duration::InfiniteFuture();
    Duration wait_duration = Duration::Millis(0);
    Duration scroll_delay = Duration::Millis(50);
    int loops  = -1;
    int dx = 1;
    int dy = 0;
    bool fit_width = false;

    int opt;
    while ((opt = getopt(argc, argv, "vg:s::w:t:c:f:b:B:hCFEd:UWa")) != -1) {
        switch (opt) {
        case 'g':
            if (sscanf(optarg, "%dx%d", &width, &height) < 2) {
                fprintf(stderr, "Invalid size spec '%s'", optarg);
                return usage(argv[0], term_width, term_height);
            }
            break;
        case 'w':
            wait_duration = Duration::Millis(roundf(atof(optarg) * 1000.0f));
            break;
        case 't':
            duration = Duration::Millis(roundf(atof(optarg) * 1000.0f));
            break;
        case 'c':
            loops = atoi(optarg);
            break;
        case 'f':
            max_frames = atoi(optarg);
            break;
        case 'a':
            do_antialias = false;
            break;
        case 'b':
            bg_color = strdup(optarg);
            break;
        case 'B':
            pattern_color = strdup(optarg);
            break;
        case 's':
            do_scroll = true;
            if (optarg != NULL) {
                scroll_delay = Duration::Millis(atoi(optarg));
            }
            break;
        case 'd':
            if (sscanf(optarg, "%d:%d", &dx, &dy) < 1) {
                fprintf(stderr, "-d%s: At least dx paramter needed e.g. -d1."
                        "Or you can give dx, dy like so: -d1:-1", optarg);
                return usage(argv[0], term_width, term_height);
            }
            break;
        case 'C':
            do_clear = true;
            break;
        case 'U':
            scale_options.do_upscale = !scale_options.do_upscale;
            break;
        case 'F':
            show_filename = !show_filename;
            break;
        case 'E':
            hide_cursor = false;
            break;
        case 'W':
            fit_width = true;
            break;
        case 'v':
            fprintf(stderr, "timg " TIMG_VERSION
                    " <https://github.com/hzeller/timg>\n"
                    "Copyright (c) 2016 Henner Zeller. "
                    "This program is free software; license GPL 2.0.\n");
            return 0;
        case 'h':
        default:
            return usage(argv[0], term_width, term_height);
        }
    }

    if (width < 1 || height < 1) {
        if (!winsize_success || term_height < 0 || term_width < 0) {
            fprintf(stderr, "Failed to read size from terminal; "
                    "Please supply -g<width>x<height> directly.\n");
        } else {
            fprintf(stderr, "%dx%d is a rather unusual size\n", width, height);
        }
        return usage(argv[0], term_width, term_height);
    }

    if (optind >= argc) {
        fprintf(stderr, "Expected image filename.\n");
        return usage(argv[0], term_width, term_height);
    }

    // There is no scroll if there is no movement.
    if (dx == 0 && dy == 0) {
        fprintf(stderr, "Scrolling chosen, but dx:dy = 0:0. "
                "Just showing image, no scroll.\n");
        do_scroll = false;
    }

    // If we scroll in one direction (so have 'infinite' space) we want fill
    // the available screen space fully in the other direction.
    scale_options.fill_width  = fit_width || (do_scroll && dy != 0);
    scale_options.fill_height = do_scroll && dx != 0; // scroll hor, fill vert
    int exit_code = 0;

    signal(SIGTERM, InterruptHandler);
    signal(SIGINT, InterruptHandler);

    timg::TerminalCanvas canvas(STDOUT_FILENO);

    for (int imgarg = optind; imgarg < argc && !interrupt_received; ++imgarg) {
        const char *filename = argv[imgarg];
        if (show_filename) {
            printf("%s\n", filename);
        }

        std::vector<Magick::Image> frames;
        bool is_animation = false;
        if (!LoadImageAndScale(filename, width, height,
                               scale_options,
                               do_antialias,
                               bg_color, pattern_color,
                               &frames, &is_animation)) {
            exit_code = 1;
            continue;
        }

        if (do_scroll && frames.size() > 1) {
            fprintf(stderr, "This is an %simage format, "
                    "scrolling on top of that is not supported. "
                    "Just doing the scrolling of the first frame.\n",
                    is_animation ? "animated " : "multi-");
            // TODO: do both.
        }

        // Adjust width/height after scaling.
        const int display_width = std::min(width, (int)frames[0].columns());
        const int display_height = std::min(height, (int)frames[0].rows());

        if (do_clear) {
            canvas.ClearScreen();
        }
        if (hide_cursor) {
            canvas.CursorOff();
        }
        if (do_scroll) {
            DisplayScrolling(frames[0], scroll_delay, duration, loops,
                             display_width, display_height, dx, dy,
                             &canvas);
        } else {
            DisplayImageSequence(frames, is_animation,
                                 duration, max_frames, loops,
                                 &canvas);
            if (frames.size() == 1) {
                (Time::Now() + wait_duration).WaitUntil();
            }
        }
        if (hide_cursor) {
            canvas.CursorOn();
        }
    }

    if (interrupt_received)   // Make 'Ctrl-C' appear on new line.
        printf("\n");

    return exit_code;
}
