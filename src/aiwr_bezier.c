/*
 * i3-aiwr — curvas de animação. Ver aiwr_bezier.h.
 */
#include "all.h"
#include "i3/aiwr_bezier.h"
#include <math.h>

#define CURVELOG(fmt, ...) LOG("[i3-aiwr] Curve: " fmt, ##__VA_ARGS__)

/* Passo e teto da busca do tempo de assentamento da mola. */
#define SPRING_STEP_S 0.001
#define SPRING_MAX_S 10.0

static aiwr_curve_t *curves;
static int num_curves;
static bool curves_ready;

/* Presets embutidos: funcionam sem nenhuma linha na config. */
static const struct {
    const char *name;
    double x1, y1, x2, y2;
} builtin_bezier[] = {
    {"default", 0.05, 0.7, 0.1, 1.0}, /* = md3_decel */
    {"linear", 0.0, 0.0, 1.0, 1.0},
    {"md3_standard", 0.2, 0.0, 0.0, 1.0},
    {"md3_decel", 0.05, 0.7, 0.1, 1.0},
    {"md3_accel", 0.3, 0.0, 0.8, 0.15},
    {"overshot", 0.05, 0.9, 0.1, 1.1},
    {"crazyshot", 0.1, 1.5, 0.76, 0.92},
    {"hyprnostretch", 0.05, 0.9, 0.1, 1.0},
    {"menu_decel", 0.1, 1.0, 0.0, 1.0},
    {"menu_accel", 0.38, 0.04, 1.0, 0.07},
    {"easeInOutCirc", 0.85, 0.0, 0.15, 1.0},
    {"easeOutCirc", 0.0, 0.55, 0.45, 1.0},
    {"easeOutExpo", 0.16, 1.0, 0.3, 1.0},
    {"softAcDecel", 0.26, 0.26, 0.15, 1.0},
    {"md2", 0.4, 0.0, 0.2, 1.0},
};

static const struct {
    const char *name;
    double damping_ratio, stiffness, epsilon;
} builtin_spring[] = {
    /* o preset do niri: crítico, sem overshoot */
    {"spring", 1.0, 1000.0, 0.0001},
    {"spring_snappy", 1.0, 1600.0, 0.0001},
    {"spring_soft", 1.0, 500.0, 0.0001},
    /* subamortecidas: passam do alvo e voltam */
    {"spring_bouncy", 0.7, 1000.0, 0.0001},
    {"spring_wobbly", 0.5, 800.0, 0.0001},
};

static double clamp01(double v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

static aiwr_curve_t *curve_find(const char *name) {
    for (int i = 0; i < num_curves; i++) {
        if (strcasecmp(curves[i].name, name) == 0) return &curves[i];
    }
    return NULL;
}

static aiwr_curve_t *curve_slot(const char *name) {
    aiwr_curve_t *c = curve_find(name);
    if (c != NULL) {
        char *keep = c->name;
        memset(c, 0, sizeof(*c));
        c->name = keep;
        return c;
    }
    curves = srealloc(curves, sizeof(aiwr_curve_t) * (num_curves + 1));
    c = &curves[num_curves++];
    memset(c, 0, sizeof(*c));
    c->name = sstrdup(name);
    return c;
}

void aiwr_curves_init(void) {
    if (curves_ready) return;
    curves_ready = true;
    for (size_t i = 0; i < sizeof(builtin_bezier) / sizeof(builtin_bezier[0]); i++) {
        aiwr_curve_define(builtin_bezier[i].name, builtin_bezier[i].x1, builtin_bezier[i].y1,
                          builtin_bezier[i].x2, builtin_bezier[i].y2);
    }
    for (size_t i = 0; i < sizeof(builtin_spring) / sizeof(builtin_spring[0]); i++) {
        aiwr_curve_define_spring(builtin_spring[i].name, builtin_spring[i].damping_ratio,
                                 builtin_spring[i].stiffness, 1.0, builtin_spring[i].epsilon, 1.0);
    }
}

bool aiwr_curve_define(const char *name, double x1, double y1, double x2, double y2) {
    if (name == NULL || *name == '\0') return false;
    aiwr_curve_t *c = curve_slot(name);
    c->type = AIWR_CURVE_BEZIER;
    /* só x precisa ser monotônico em [0,1]; y é livre (overshoot) */
    c->x1 = clamp01(x1);
    c->y1 = y1;
    c->x2 = clamp01(x2);
    c->y2 = y2;
    return true;
}

/* -------------------------------------------------------------- bezier */

/* x(t) e y(t) com P0 = (0,0), P3 = (1,1) */
static double bez(double a, double b, double t) {
    double u = 1.0 - t;
    return 3.0 * u * u * t * a + 3.0 * u * t * t * b + t * t * t;
}

static double bez_dx(double x1, double x2, double t) {
    double u = 1.0 - t;
    return 3.0 * u * u * x1 + 6.0 * u * t * (x2 - x1) + 3.0 * t * t * (1.0 - x2);
}

static double bezier_eval(const aiwr_curve_t *c, double p) {
    /* Newton para achar t tal que x(t) = p; bissecção se derivar mal */
    double t = p;
    for (int i = 0; i < 8; i++) {
        double x = bez(c->x1, c->x2, t) - p;
        if (fabs(x) < 1e-6) return bez(c->y1, c->y2, t);
        double d = bez_dx(c->x1, c->x2, t);
        if (fabs(d) < 1e-6) break;
        t -= x / d;
        if (t < 0.0) t = 0.0;
        if (t > 1.0) t = 1.0;
    }
    double lo = 0.0, hi = 1.0;
    t = p;
    for (int i = 0; i < 24; i++) {
        double x = bez(c->x1, c->x2, t);
        if (fabs(x - p) < 1e-6) break;
        if (x < p) lo = t; else hi = t;
        t = (lo + hi) / 2.0;
    }
    return bez(c->y1, c->y2, t);
}

/* -------------------------------------------------------------- spring */

/* Oscilador harmônico amortecido de 0 a 1, velocidade inicial zero.
 * t em segundos. Mesma formulação do libadwaita/niri. */
static double spring_at(const aiwr_curve_t *c, double t) {
    double m = (c->mass > 0.0) ? c->mass : 1.0;
    double k = c->stiffness;
    double damping = c->damping_ratio * 2.0 * sqrt(m * k);
    double b = damping / (2.0 * m);
    double omega0 = sqrt(k / m);
    double x0 = -1.0; /* from - to */
    double envelope = exp(-b * t);

    if (fabs(c->damping_ratio - 1.0) < 1e-9) { /* crítico */
        return 1.0 + envelope * (x0 + b * x0 * t);
    }
    if (c->damping_ratio > 1.0) { /* sobreamortecido */
        double omega1 = sqrt(b * b - omega0 * omega0);
        return 1.0 + envelope * (x0 * cosh(omega1 * t) + (b * x0 / omega1) * sinh(omega1 * t));
    }
    /* subamortecido: passa do alvo e volta */
    double omega2 = sqrt(omega0 * omega0 - b * b);
    return 1.0 + envelope * (x0 * cos(omega2 * t) + (b * x0 / omega2) * sin(omega2 * t));
}

/* Anda no tempo até a mola parar de sair de epsilon. Roda uma vez, quando a
 * curva é definida — não por frame. */
static double spring_settle_ms(const aiwr_curve_t *c) {
    double eps = (c->epsilon > 0.0) ? c->epsilon : 0.0001;
    double last = 0.0;
    for (double t = 0.0; t < SPRING_MAX_S; t += SPRING_STEP_S) {
        if (fabs(spring_at(c, t) - 1.0) > eps) last = t;
    }
    return (last + SPRING_STEP_S) * 1000.0;
}

bool aiwr_curve_define_spring(const char *name, double damping_ratio, double stiffness,
                              double mass, double epsilon, double speed) {
    if (name == NULL || *name == '\0') return false;
    if (stiffness <= 0.0) {
        ELOG("Curve: spring '%s' needs stiffness > 0\n", name);
        return false;
    }
    if (damping_ratio <= 0.0) damping_ratio = 1.0;
    if (mass <= 0.0) mass = 1.0;
    if (epsilon <= 0.0) epsilon = 0.0001;
    if (speed <= 0.0) speed = 1.0;

    aiwr_curve_t *c = curve_slot(name);
    c->type = AIWR_CURVE_SPRING;
    c->damping_ratio = damping_ratio;
    c->stiffness = stiffness;
    c->mass = mass;
    c->epsilon = epsilon;
    c->settle_ms = spring_settle_ms(c);
    return true;
}

/* ---------------------------------------------------------------- specs */

/* Troca vírgulas por espaços, para aceitar os dois estilos de config. */
static void spec_normalize(const char *spec, char *buf, size_t cap) {
    size_t n = 0;
    for (const char *p = spec; *p != '\0' && n < cap - 1; p++) {
        buf[n++] = (*p == ',') ? ' ' : *p;
    }
    buf[n] = '\0';
}

bool aiwr_curve_define_spec(const char *spec) {
    if (spec == NULL) return false;
    char buf[256];
    spec_normalize(spec, buf, sizeof(buf));
    char name[64];
    double x1, y1, x2, y2;
    if (sscanf(buf, "%63s %lf %lf %lf %lf", name, &x1, &y1, &x2, &y2) != 5) {
        ELOG("Curve: could not parse bezier definition '%s'\n", spec);
        return false;
    }
    aiwr_curves_init();
    bool ok = aiwr_curve_define(name, x1, y1, x2, y2);
    if (ok) CURVELOG("bezier %s = (%.3f, %.3f, %.3f, %.3f)\n", name, x1, y1, x2, y2);
    return ok;
}

bool aiwr_curve_define_spring_spec(const char *spec) {
    if (spec == NULL) return false;
    char buf[256];
    spec_normalize(spec, buf, sizeof(buf));

    char *save = NULL;
    char *tok = strtok_r(buf, " \t", &save);
    if (tok == NULL) {
        ELOG("Curve: spring definition needs a name\n");
        return false;
    }
    char name[64];
    snprintf(name, sizeof(name), "%s", tok);

    double damping_ratio = 1.0, stiffness = 1000.0, mass = 1.0, epsilon = 0.0001, speed = 1.0;
    bool got_stiffness = false;
    while ((tok = strtok_r(NULL, " \t", &save)) != NULL) {
        char *eq = strchr(tok, '=');
        if (eq == NULL) {
            ELOG("Curve: spring '%s': expected key=value, got '%s'\n", name, tok);
            return false;
        }
        *eq = '\0';
        double v = strtod(eq + 1, NULL);
        /* aceita hífen e underscore: damping-ratio e damping_ratio */
        if (strcasecmp(tok, "damping-ratio") == 0 || strcasecmp(tok, "damping_ratio") == 0) {
            damping_ratio = v;
        } else if (strcasecmp(tok, "stiffness") == 0) {
            stiffness = v;
            got_stiffness = true;
        } else if (strcasecmp(tok, "epsilon") == 0) {
            epsilon = v;
        } else if (strcasecmp(tok, "mass") == 0) {
            mass = v;
        } else if (strcasecmp(tok, "speed") == 0) {
            speed = v;
        } else {
            ELOG("Curve: spring '%s': unknown key '%s'\n", name, tok);
            return false;
        }
    }
    if (!got_stiffness) {
        CURVELOG("spring %s: no stiffness given, using %.0f\n", name, stiffness);
    }

    aiwr_curves_init();
    if (!aiwr_curve_define_spring(name, damping_ratio, stiffness, mass, epsilon, speed)) return false;
    const aiwr_curve_t *c = curve_find(name);
    CURVELOG("spring %s = damping-ratio %.3f stiffness %.0f mass %.2f epsilon %g -> %.0f ms\n",
             name, damping_ratio, stiffness, mass, epsilon, c ? c->settle_ms : 0.0);
    return true;
}

int aiwr_curve_count(void) {
    aiwr_curves_init();
    return num_curves;
}

const char *aiwr_curve_name_at(int i) {
    aiwr_curves_init();
    return (i >= 0 && i < num_curves) ? curves[i].name : NULL;
}

const aiwr_curve_t *aiwr_curve_get(const char *name) {
    if (name == NULL) return NULL;
    aiwr_curves_init();
    return curve_find(name);
}

double aiwr_curve_natural_duration_ms(const aiwr_curve_t *c) {
    if (c == NULL || c->type != AIWR_CURVE_SPRING) return 0.0;
    return c->settle_ms;
}

double aiwr_curve_eval(const aiwr_curve_t *c, double p) {
    p = clamp01(p);
    if (c == NULL) { /* ease-out-cubic */
        double u = 1.0 - p;
        return 1.0 - u * u * u;
    }
    if (p <= 0.0) return 0.0;
    if (p >= 1.0) return 1.0;

    if (c->type == AIWR_CURVE_SPRING) {
        double dur_s = c->settle_ms / 1000.0;
        if (dur_s <= 0.0) return 1.0;
        return spring_at(c, p * dur_s);
    }
    return bezier_eval(c, p);
}

double aiwr_curve_eval_named(const char *name, double p) {
    return aiwr_curve_eval(aiwr_curve_get(name), p);
}
