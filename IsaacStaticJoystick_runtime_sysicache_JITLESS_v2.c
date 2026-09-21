/*
 * IsaacStaticJoystick.dylib (variante configurable en runtime)
 * ---------------------------------------------------------
 * Misma infraestructura genérica que la variante de placeholders
 * en tiempo de compilación (UUID lookup, ASLR slide, verificación
 * fail-closed, hardware breakpoints, logs, constructor),
 * pero los 7 parámetros de configuración:
 *
 *   target_uuid
 *   patch1_offset, patch1_original, patch1_new
 *   patch2_offset, patch2_original, patch2_new
 *
 * NO son macros/constantes de compilación: se leen desde un
 * archivo de texto externo en tiempo de ejecución. Esto evita que
 * el compilador pueda hacer constant-folding y eliminar la lógica
 * de patching quede pase lo que pase con los valores (incluso si
 * el archivo de config no existe o está vacío, la lógica de
 * lectura/parsing/verificación sigue presente en el binario;
 * simplemente no encontrará valores validos y no hará nada).
 *
 * Este .c es de nuevo freestanding (sin headers de sistema) porque
 * este sandbox no tiene un SDK de iOS instalado.
 * ---------------------------------------------------------
 */

/* ===================== Tipos basicos ===================== */
typedef unsigned char      uint8_t;
typedef unsigned int       uint32_t;
typedef int                int32_t;
typedef unsigned long      uintptr_t;
typedef long                intptr_t;
typedef unsigned long      uint64_t;
typedef unsigned long      size_t;
typedef long                ssize_t;
typedef unsigned int       useconds_t;
typedef uint8_t            uuid_t[16];

/* ===================== Prototipos dyld ===================== */
struct mach_header; /* tipo opaco, solo usado como puntero */

extern uint32_t _dyld_image_count(void);
extern const struct mach_header *_dyld_get_image_header(uint32_t image_index);
/* Firma REAL de la API dyld: recibe un buffer uuid_t como segundo
 * argumento (out-param) y devuelve exito/error como bool/int, NO
 * un puntero. La declaracion anterior era incorrecta. */
extern int _dyld_get_image_uuid(const struct mach_header *mh, uuid_t uuid);
extern intptr_t _dyld_get_image_vmaddr_slide(uint32_t image_index);

/* ===================== Prototipos libc/libSystem ===================== */
extern long write(int fd, const void *buf, size_t count);
extern int usleep(useconds_t usec);
extern long sysconf(int name);
extern void sys_icache_invalidate(void *start, size_t len);
extern int open(const char *path, int oflag, ...);
extern ssize_t read(int fd, void *buf, size_t count);
extern int close(int fd);
extern char *getenv(const char *name);

#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define PROT_EXEC   0x4
#define VM_PROT_COPY 0x10
#define SIGTRAP 5

/* Mach thread state constants used by LiveContainer's JIT-less path. */
#define ARM_THREAD_STATE64 6
#define ARM_THREAD_STATE64_COUNT 68
#define ARM_DEBUG_STATE64 15
#define ARM_DEBUG_STATE64_COUNT 130

/* Leave the lowest two breakpoint slots alone: LiveContainer may use them
 * for its own JIT-less dyld hook. */
#define HW_BP_SLOT1 2
#define HW_BP_SLOT2 3

typedef uint32_t thread_t;
typedef uint32_t thread_state_flavor_t;
typedef uint32_t mach_msg_type_number_t;
#define KERN_SUCCESS 0

typedef struct {
    uint64_t x[29];
    uint64_t fp;
    uint64_t lr;
    uint64_t sp;
    uint64_t pc;
    uint32_t cpsr;
    uint32_t pad;
} arm_thread_state64_min_t;

typedef struct {
    uint64_t bvr[16];
    uint64_t bcr[16];
    uint64_t wvr[16];
    uint64_t wcr[16];
    uint64_t mdscr_el1;
} arm_debug_state64_min_t;

extern thread_t mach_thread_self(void);
extern int thread_get_state(thread_t target, thread_state_flavor_t flavor, void *state, mach_msg_type_number_t *count);
extern int thread_set_state(thread_t target, thread_state_flavor_t flavor, const void *state, mach_msg_type_number_t count);

/* Minimal Darwin signal/ucontext ABI definitions. These match the arm64
 * layout used by iOS, but keep the source freestanding. */
typedef unsigned int isaac_sigset_t;

typedef struct {
    void *ss_sp;
    size_t ss_size;
    int ss_flags;
    int _pad;
} isaac_sigaltstack_t;

typedef struct isaac_ucontext isaac_ucontext_t;

typedef struct {
    uint64_t __x[29];
    uint64_t __fp;
    uint64_t __lr;
    uint64_t __sp;
    uint64_t __pc;
    uint32_t __cpsr;
    uint32_t __pad;
} isaac_arm_thread_state64_t;

typedef struct {
    uint64_t __far;
    uint32_t __esr;
    uint32_t __exception;
} isaac_arm_exception_state64_t;

typedef struct {
    isaac_arm_exception_state64_t __es;
    isaac_arm_thread_state64_t __ss;
} isaac_mcontext64_prefix_t;

struct isaac_ucontext {
    int uc_onstack;
    isaac_sigset_t uc_sigmask;
    isaac_sigaltstack_t uc_stack;
    isaac_ucontext_t *uc_link;
    size_t uc_mcsize;
    isaac_mcontext64_prefix_t *uc_mcontext;
};

typedef union {
    void (*__sa_handler)(int);
    void (*__sa_sigaction)(int, void *, void *);
} isaac_sigaction_u_t;

typedef struct {
    isaac_sigaction_u_t __sigaction_u;
    isaac_sigset_t sa_mask;
    int sa_flags;
} isaac_sigaction_t;

extern int sigaction(int sig, const isaac_sigaction_t *act, isaac_sigaction_t *oldact);

#define SA_NODEFER 0x0010
#define SA_SIGINFO 0x0040

#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)

#define _SC_PAGESIZE 29
#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_CREAT  0x0200
#define O_TRUNC  0x0400

/* ===================== Mach thread/debug primitives ===================== */
/* ===================== Utilidades basicas (sin libc) ===================== */

static size_t my_strlen(const char *s) {
    size_t n = 0;
    while (s[n] != '\0') n++;
    return n;
}

static int my_memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i]) return (int)pa[i] - (int)pb[i];
    }
    return 0;
}

/* ============================================================
 *  Buffer de reporte: acumula TODO lo que se loguea, para poder
 *  volcarlo a un archivo en disco al final del constructor (ademas
 *  de seguir mandandolo a stderr, por si algo lo captura).
 * ============================================================ */
#define REPORT_BUF_SIZE 16384
static char g_report_buf[REPORT_BUF_SIZE];
static size_t g_report_len = 0;

static void report_append(const char *msg) {
    size_t n = my_strlen(msg);
    if (g_report_len + n >= REPORT_BUF_SIZE) return; /* truncar silenciosamente */
    for (size_t i = 0; i < n; i++) g_report_buf[g_report_len + i] = msg[i];
    g_report_len += n;
    g_report_buf[g_report_len] = '\0';
}

static void log_msg(const char *msg) {
    write(2, msg, my_strlen(msg));
    report_append(msg);
}

#define REPORT_FILENAME "IsaacStaticJoystick_report.txt"

/* Forward declarations (definidas mas abajo, junto al resto de utilidades) */
static void my_strcpy(char *dst, const char *src);
static void my_strcat(char *dst, const char *src);

/* Vuelca g_report_buf a $HOME/IsaacStaticJoystick_report.txt.
 * Usa el mismo mecanismo confirmado como escribible (Diag3):
 * ruta directa en $HOME, sin subcarpetas. */
static void flush_report_to_disk(void) {
    const char *home = getenv("HOME");
    if (!home) return;

    char path[1024];
    my_strcpy(path, home);
    my_strcat(path, "/" REPORT_FILENAME);

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        write(fd, g_report_buf, g_report_len);
        close(fd);
    }
}

/* ===================== Ruta del archivo de configuración =====================
 *
 * La ruta ya NO es una constante de compilación fija: se construye en
 * runtime como $HOME/IsaacStaticJoystick.cfg (directamente en la raíz
 * del contenedor de datos de la app, NO dentro de Documents/), porque
 * fue el único lugar confirmado como escribible y accesible en este
 * entorno de LiveContainer mediante las pruebas de bisección previas
 * (IsaacStaticJoystickDiag3). $HOME se resuelve vía getenv("HOME") en
 * tiempo de ejecución, así que esto sigue sin ser un valor específico
 * de ningún binario objetivo: es solo la ubicación del archivo de
 * configuración dentro del propio sandbox de la app.
 */
#define CONFIG_FILENAME "IsaacStaticJoystick.cfg"
#define CONFIG_MAX_SIZE 4096

/* ===================== Parser de config minimo =====================
 *
 * Formato de texto, una asignacion por linea:
 *
 *   target_uuid=XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX
 *   patch1_offset=0xNNNNNN
 *   patch1_original=0xNNNNNNNN
 *   patch1_new=0xNNNNNNNN
 *   patch2_offset=0xNNNNNN
 *   patch2_original=0xNNNNNNNN
 *   patch2_new=0xNNNNNNNN
 *
 * Líneas vacías o que no matchean una clave conocida se ignoran.
 * No se contempla ningún valor por defecto para estos 7 campos:
 * si faltan, la validación de "config completa" falla y el módulo
 * queda inerte (igual filosofía fail-closed que el resto del código).
 */

typedef struct {
    int has_uuid;
    uuid_t target_uuid;

    int has_offset1;
    uintptr_t patch1_offset;
    int has_original1;
    uint32_t patch1_original;
    int has_new1;
    uint32_t patch1_new;

    int has_offset2;
    uintptr_t patch2_offset;
    int has_original2;
    uint32_t patch2_original;
    int has_new2;
    uint32_t patch2_new;
} runtime_config_t;

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parsea "0xNNNN..." o "NNNN..." (hex) hasta el primer caracter no
 * valido. Devuelve 1 si se parseo al menos un digito, 0 si no. */
static int parse_hex_u64(const char *s, size_t len, unsigned long *out) {
    size_t i = 0;
    if (len >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) i = 2;

    unsigned long val = 0;
    int any = 0;
    for (; i < len; i++) {
        int n = hex_nibble(s[i]);
        if (n < 0) break;
        val = (val << 4) | (unsigned long)n;
        any = 1;
    }
    if (any) *out = val;
    return any;
}

/* Parsea un UUID con formato XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX
 * (36 caracteres, guiones en posiciones estandar) hacia 16 bytes. */
static int parse_uuid(const char *s, size_t len, uuid_t out) {
    if (len < 36) return 0;

    static const int dash_positions[4] = {8, 13, 18, 23};
    for (int d = 0; d < 4; d++) {
        if (s[dash_positions[d]] != '-') return 0;
    }

    size_t src = 0;
    size_t dst = 0;
    while (dst < 16) {
        if (s[src] == '-') { src++; continue; }
        int hi = hex_nibble(s[src]);
        int lo = hex_nibble(s[src + 1]);
        if (hi < 0 || lo < 0) return 0;
        out[dst] = (uint8_t)((hi << 4) | lo);
        src += 2;
        dst++;
    }
    return 1;
}

static int line_starts_with(const char *line, size_t line_len, const char *key) {
    size_t klen = my_strlen(key);
    if (line_len < klen) return 0;
    for (size_t i = 0; i < klen; i++) {
        if (line[i] != key[i]) return 0;
    }
    return 1;
}

/* Procesa el buffer completo de config linea por linea. */
static void parse_config_buffer(const char *buf, size_t buf_len, runtime_config_t *cfg) {
    size_t i = 0;
    while (i < buf_len) {
        size_t line_start = i;
        while (i < buf_len && buf[i] != '\n') i++;
        size_t line_end = i; /* exclusive */
        if (i < buf_len) i++; /* saltar '\n' */

        size_t line_len = line_end - line_start;
        const char *line = buf + line_start;

        /* recortar '\r' final si existe */
        if (line_len > 0 && line[line_len - 1] == '\r') line_len--;

        /* saltar lineas vacias o comentarios */
        if (line_len == 0 || line[0] == '#') continue;

        /* buscar '=' */
        size_t eq = 0;
        int found_eq = 0;
        for (size_t j = 0; j < line_len; j++) {
            if (line[j] == '=') { eq = j; found_eq = 1; break; }
        }
        if (!found_eq) continue;

        const char *key = line;
        size_t key_len = eq;
        const char *val = line + eq + 1;
        size_t val_len = line_len - eq - 1;

        (void)key_len;

        unsigned long tmp;

        if (line_starts_with(key, key_len, "target_uuid")) {
            if (parse_uuid(val, val_len, cfg->target_uuid)) cfg->has_uuid = 1;
        } else if (line_starts_with(key, key_len, "patch1_offset")) {
            if (parse_hex_u64(val, val_len, &tmp)) { cfg->patch1_offset = (uintptr_t)tmp; cfg->has_offset1 = 1; }
        } else if (line_starts_with(key, key_len, "patch1_original")) {
            if (parse_hex_u64(val, val_len, &tmp)) { cfg->patch1_original = (uint32_t)tmp; cfg->has_original1 = 1; }
        } else if (line_starts_with(key, key_len, "patch1_new")) {
            if (parse_hex_u64(val, val_len, &tmp)) { cfg->patch1_new = (uint32_t)tmp; cfg->has_new1 = 1; }
        } else if (line_starts_with(key, key_len, "patch2_offset")) {
            if (parse_hex_u64(val, val_len, &tmp)) { cfg->patch2_offset = (uintptr_t)tmp; cfg->has_offset2 = 1; }
        } else if (line_starts_with(key, key_len, "patch2_original")) {
            if (parse_hex_u64(val, val_len, &tmp)) { cfg->patch2_original = (uint32_t)tmp; cfg->has_original2 = 1; }
        } else if (line_starts_with(key, key_len, "patch2_new")) {
            if (parse_hex_u64(val, val_len, &tmp)) { cfg->patch2_new = (uint32_t)tmp; cfg->has_new2 = 1; }
        }
    }
}

static void my_strcpy(char *dst, const char *src) {
    size_t i = 0;
    while (src[i] != '\0') { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void my_strcat(char *dst, const char *src) {
    size_t d = my_strlen(dst);
    size_t i = 0;
    while (src[i] != '\0') { dst[d + i] = src[i]; i++; }
    dst[d + i] = '\0';
}

/* Construye $HOME/IsaacStaticJoystick.cfg en 'out' (buffer >= 1024).
 * Devuelve 1 si pudo construir la ruta (HOME disponible), 0 si no. */
static int build_config_path(char *out) {
    const char *home = getenv("HOME");
    if (!home) return 0;
    my_strcpy(out, home);
    my_strcat(out, "/" CONFIG_FILENAME);
    return 1;
}

/* Lee el archivo de config completo hacia un buffer estatico.
 * Devuelve la cantidad de bytes leidos, o -1 en error/no existe. */
static long read_config_file(char *buf, size_t buf_cap) {
    char config_path[1024];
    if (!build_config_path(config_path)) {
        log_msg("[IsaacStaticJoystick] HOME not set; cannot locate config\n");
        return -1;
    }

    int fd = open(config_path, O_RDONLY);
    if (fd < 0) return -1;

    long total = 0;
    while ((size_t)total < buf_cap) {
        ssize_t n = read(fd, buf + total, buf_cap - (size_t)total);
        if (n <= 0) break;
        total += n;
    }
    close(fd);
    return total;
}

static int config_is_complete(const runtime_config_t *cfg) {
    return cfg->has_uuid &&
           cfg->has_offset1 && cfg->has_original1 && cfg->has_new1 &&
           cfg->has_offset2 && cfg->has_original2 && cfg->has_new2;
}

/* ============================================================
 *  Localizacion de imagen por UUID (identico a la variante base)
 * ============================================================ */

static int uuid_matches(const uint8_t *a, const uint8_t *b) {
    return my_memcmp(a, b, 16) == 0;
}

/* ---- utilidades de formateo para los mensajes de checkpoint ---- */

static int u64_to_dec(unsigned long v, char *buf) {
    char tmp[32];
    int n = 0;
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return 1; }
    while (v > 0) { tmp[n++] = '0' + (v % 10); v /= 10; }
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = '\0';
    return n;
}

static const char HEX_DIGITS_RT[] = "0123456789ABCDEF";

static void u64_to_hex(unsigned long v, char *buf) {
    char tmp[20];
    int n = 0;
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    while (v > 0) { tmp[n++] = HEX_DIGITS_RT[v & 0xF]; v >>= 4; }
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = '\0';
}


static void checkpoint(const char *msg) {
    log_msg(msg);
    flush_report_to_disk();
}

/*
 * find_target_image() — version instrumentada con checkpoints.
 * Cada paso individual se loguea y se vuelca a disco de inmediato
 * (flush_report_to_disk dentro de checkpoint()), de modo que si el
 * proceso crashea en cualquier punto, el archivo de reporte en disco
 * ya contiene el ultimo checkpoint alcanzado.
 */
static int find_target_image(const uuid_t target_uuid, uintptr_t *out_base, intptr_t *out_slide) {
    uint32_t count = _dyld_image_count();
    {
        char numbuf[32];
        char line[128];
        u64_to_dec((unsigned long)count, numbuf);
        my_strcpy(line, "[IsaacStaticJoystick] image_count = ");
        my_strcat(line, numbuf);
        my_strcat(line, "\n");
        checkpoint(line);
    }

    for (uint32_t i = 0; i < count; i++) {
        /* Checkpoint de progreso: SOLO cada 100 imagenes (0, 100, 200, ...),
         * no una linea por imagen. Reduce el tamaño del reporte de ~1134
         * lineas detalladas a ~12. */
        if (i % 100 == 0) {
            char idxbuf[32];
            char line[96];
            u64_to_dec((unsigned long)i, idxbuf);
            my_strcpy(line, "[IsaacStaticJoystick] scanning... index=");
            my_strcat(line, idxbuf);
            my_strcat(line, "\n");
            checkpoint(line);
        }

        /* Busqueda SILENCIOSA: sin checkpoint por cada header/uuid. */
        const struct mach_header *hdr = _dyld_get_image_header(i);
        if (!hdr) continue;

        uuid_t img_uuid;
        int got_uuid = _dyld_get_image_uuid(hdr, img_uuid);
        if (!got_uuid) continue;

        if (uuid_matches((const uint8_t *)img_uuid, target_uuid)) {
            char idxbuf[32];
            char line[96];
            u64_to_dec((unsigned long)i, idxbuf);
            my_strcpy(line, "[IsaacStaticJoystick] UUID MATCHED at image index ");
            my_strcat(line, idxbuf);
            my_strcat(line, "\n");
            checkpoint(line);

            *out_base = (uintptr_t)hdr;
            *out_slide = _dyld_get_image_vmaddr_slide(i);
            return 1;
        }
    }

    checkpoint("[IsaacStaticJoystick] UUID NOT FOUND\n");
    return 0;
}

/* ============================================================
 *  JIT-less runtime patching via ARM64 hardware breakpoints
 * ============================================================
 *
 * iOS 26.1 rejects execution from a guest __TEXT page that has been
 * modified with mprotect()/vm_protect(), even when VM_PROT_COPY is used.
 * Do not modify executable memory at all. Instead, arm two hardware
 * instruction breakpoints and emulate the configured replacement
 * instruction when the CPU reaches the original address.
 *
 * This keeps the tweak JIT-less and leaves the guest __TEXT mapping RX.
 * The implementation intentionally supports a conservative subset of
 * AArch64 instructions. If a replacement opcode is unsupported, that
 * breakpoint is disabled and the original instruction is emulated so the
 * game continues instead of crashing.
 */

static uintptr_t g_hw_addr1 = 0;
static uintptr_t g_hw_addr2 = 0;
static uint32_t g_hw_orig1 = 0;
static uint32_t g_hw_orig2 = 0;
static uint32_t g_hw_new1 = 0;
static uint32_t g_hw_new2 = 0;
static int g_hw_enabled1 = 0;
static int g_hw_enabled2 = 0;
static volatile uint32_t g_hw_hits1 = 0;
static volatile uint32_t g_hw_hits2 = 0;

static long sign_extend(unsigned long value, int bits) {
    unsigned long mask = 1UL << (bits - 1);
    unsigned long full = (1UL << bits) - 1UL;
    value &= full;
    return (value & mask) ? (long)(value | ~full) : (long)value;
}

static int emulate_a64(uint32_t insn, arm_thread_state64_min_t *st, uintptr_t pc) {
    /* NOP */
    if (insn == 0xD503201F) {
        st->pc = pc + 4;
        return 1;
    }

    /* RET / RETAA / RETAB-like plain RET encoding only. */
    if ((insn & 0xFFFFFC1F) == 0xD65F0000) {
        unsigned int rn = (insn >> 5) & 0x1F;
        if (rn < 31) st->pc = st->x[rn];
        else st->pc = st->lr;
        return 1;
    }

    /* B / BL immediate. */
    if ((insn & 0x7C000000) == 0x14000000) {
        unsigned long imm26 = insn & 0x03FFFFFFUL;
        long delta = sign_extend(imm26, 26) << 2;
        if (insn & 0x80000000U) st->lr = pc + 4;
        st->pc = (uintptr_t)((long)pc + delta);
        return 1;
    }

    /* ADD/SUB (immediate), 32- or 64-bit, flags not requested. */
    if ((insn & 0x1F000000) == 0x11000000 && (insn & (1U << 29)) == 0) {
        unsigned int sf = (insn >> 31) & 1U;
        unsigned int op = (insn >> 30) & 1U;
        unsigned int sh = (insn >> 22) & 1U;
        unsigned long imm12 = (insn >> 10) & 0xFFFUL;
        unsigned int rn = (insn >> 5) & 0x1F;
        unsigned int rd = insn & 0x1F;
        unsigned long imm = sh ? (imm12 << 12) : imm12;
        unsigned long lhs = (rn == 31) ? st->sp : st->x[rn];
        unsigned long result;

        if (!sf) {
            uint32_t a = (uint32_t)lhs;
            uint32_t b = (uint32_t)imm;
            uint32_t r = op ? (a - b) : (a + b);
            if (rd == 31) st->sp = (uint64_t)r;
            else st->x[rd] = (uint64_t)r;
        } else {
            result = op ? (lhs - imm) : (lhs + imm);
            if (rd == 31) st->sp = result;
            else st->x[rd] = result;
        }

        st->pc = pc + 4;
        return 1;
    }

    /* ADR / ADRP. */
    if ((insn & 0x9F000000) == 0x10000000 ||
        (insn & 0x9F000000) == 0x90000000) {
        unsigned long immlo = (insn >> 29) & 0x3UL;
        unsigned long immhi = (insn >> 5) & 0x7FFFFUL;
        unsigned long raw = (immhi << 2) | immlo;
        long delta;
        unsigned int rd = insn & 0x1F;
        if ((insn & 0x9F000000) == 0x90000000) {
            delta = sign_extend(raw, 21) << 12;
            st->x[rd] = ((uint64_t)pc & ~0xFFFUL) + delta;
        } else {
            delta = sign_extend(raw, 21);
            st->x[rd] = (uint64_t)((long)pc + delta);
        }
        st->pc = pc + 4;
        return 1;
    }

    /* MOVZ / MOVN / MOVK. */
    if ((insn & 0x1F800000) == 0x12800000 ||
        (insn & 0x1F800000) == 0x52800000 ||
        (insn & 0x1F800000) == 0x72800000) {
        unsigned int sf = (insn >> 31) & 1U;
        unsigned int opc = (insn >> 29) & 0x3U;
        unsigned int hw = (insn >> 21) & 0x3U;
        unsigned long imm16 = (insn >> 5) & 0xFFFFUL;
        unsigned int rd = insn & 0x1F;
        unsigned int shift = hw * 16;
        unsigned long mask = sf ? ~0UL : 0xFFFFFFFFUL;
        unsigned long v = (imm16 << shift) & mask;
        unsigned long old = (rd == 31) ? 0 : st->x[rd];

        if (opc == 2) { /* MOVZ */
            old = v;
        } else if (opc == 0) { /* MOVN */
            old = (~v) & mask;
        } else if (opc == 3) { /* MOVK */
            unsigned long chunk_mask = (0xFFFFUL << shift) & mask;
            old = (old & ~chunk_mask) | v;
        } else {
            return 0;
        }

        if (!sf) old &= 0xFFFFFFFFUL;
        st->x[rd] = old;
        st->pc = pc + 4;
        return 1;
    }

    /* CBZ / CBNZ (register value read only). */
    if ((insn & 0x7E000000) == 0x34000000) {
        unsigned int sf = (insn >> 31) & 1U;
        unsigned int nz = (insn >> 24) & 1U;
        unsigned long imm19 = (insn >> 5) & 0x7FFFFUL;
        unsigned int rt = insn & 0x1F;
        unsigned long v = (rt == 31) ? 0 : st->x[rt];
        if (!sf) v &= 0xFFFFFFFFUL;
        long delta = sign_extend(imm19, 19) << 2;
        int is_zero = (v == 0);
        if ((nz && !is_zero) || (!nz && is_zero)) st->pc = (uintptr_t)((long)pc + delta);
        else st->pc = pc + 4;
        return 1;
    }

    /* TBZ / TBNZ. */
    if ((insn & 0x7E000000) == 0x36000000) {
        unsigned int nz = (insn >> 24) & 1U;
        unsigned int bit = ((insn >> 31) & 1U) << 5 | ((insn >> 19) & 0x1FU);
        unsigned long imm14 = (insn >> 5) & 0x3FFFUL;
        unsigned int rt = insn & 0x1F;
        unsigned long v = (rt == 31) ? 0 : st->x[rt];
        int set = (int)((v >> bit) & 1UL);
        long delta = sign_extend(imm14, 14) << 2;
        if ((nz && set) || (!nz && !set)) st->pc = (uintptr_t)((long)pc + delta);
        else st->pc = pc + 4;
        return 1;
    }

    return 0;
}

/* IMPORTANT: a POSIX signal handler must modify the saved ucontext.
 * Calling thread_set_state() on the current thread does NOT update the
 * signal frame that the trampoline restores on return; the old version
 * therefore re-entered the same hardware breakpoint forever. */

static void hw_breakpoint_signal_handler(int sig, void *info, void *vctx) {
    (void)sig;
    (void)info;

    isaac_ucontext_t *ctx = (isaac_ucontext_t *)vctx;
    if (!ctx || !ctx->uc_mcontext) return;

    isaac_arm_thread_state64_t *st = &ctx->uc_mcontext->__ss;
    uintptr_t pc = (uintptr_t)st->__pc;

    uint32_t original = 0;
    uint32_t replacement = 0;

    if (g_hw_enabled1 && pc == g_hw_addr1) {
        original = g_hw_orig1;
        replacement = g_hw_new1;
        g_hw_hits1++;
    } else if (g_hw_enabled2 && pc == g_hw_addr2) {
        original = g_hw_orig2;
        replacement = g_hw_new2;
        g_hw_hits2++;
    } else {
        return;
    }

    uint32_t current = *(volatile uint32_t *)pc;
    if (current != original) return;

    /* Emulate directly against the signal's saved register state. This is
     * the state iOS will restore when the handler returns. */
    arm_thread_state64_min_t emu;
    for (int i = 0; i < 29; i++) emu.x[i] = st->__x[i];
    emu.fp = st->__fp;
    emu.lr = st->__lr;
    emu.sp = st->__sp;
    emu.pc = st->__pc;
    emu.cpsr = st->__cpsr;
    emu.pad = st->__pad;

    if (!emulate_a64(replacement, &emu, pc)) {
        return;
    }

    for (int i = 0; i < 29; i++) st->__x[i] = emu.x[i];
    st->__fp = emu.fp;
    st->__lr = emu.lr;
    st->__sp = emu.sp;
    st->__pc = emu.pc;
    st->__cpsr = emu.cpsr;
    st->__pad = emu.pad;
}

static int arm_hardware_breakpoints(uintptr_t addr1, uint32_t original1, uint32_t new1,
                                    uintptr_t addr2, uint32_t original2, uint32_t new2) {
    thread_t self = mach_thread_self();
    arm_debug_state64_min_t dbg;
    mach_msg_type_number_t count = ARM_DEBUG_STATE64_COUNT;

    if (!self) return -1;

    int kr = thread_get_state(self, ARM_DEBUG_STATE64, &dbg, &count);
    if (kr != KERN_SUCCESS) return -1;

    /* Install an SA_SIGINFO handler. The handler edits the saved ucontext,
     * which is what iOS restores when SIGTRAP returns. */
    isaac_sigaction_t sa;
    sa.__sigaction_u.__sa_sigaction = hw_breakpoint_signal_handler;
    sa.sa_mask = 0;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    if (sigaction(SIGTRAP, &sa, 0) != 0) {
        return -1;
    }

    g_hw_addr1 = addr1;
    g_hw_addr2 = addr2;
    g_hw_orig1 = original1;
    g_hw_orig2 = original2;
    g_hw_new1 = new1;
    g_hw_new2 = new2;
    g_hw_enabled1 = 1;
    g_hw_enabled2 = 1;

    /* Preserve all non-owned debug registers; use slots 2 and 3 so the
     * lowest slots remain available to LiveContainer's own JIT-less hooks. */
    dbg.bvr[HW_BP_SLOT1] = (uint64_t)addr1;
    dbg.bcr[HW_BP_SLOT1] = 0x1E5;
    dbg.bvr[HW_BP_SLOT2] = (uint64_t)addr2;
    dbg.bcr[HW_BP_SLOT2] = 0x1E5;

    kr = thread_set_state(self, ARM_DEBUG_STATE64, &dbg, ARM_DEBUG_STATE64_COUNT);
    if (kr != KERN_SUCCESS) {
        g_hw_enabled1 = 0;
        g_hw_enabled2 = 0;
        return -1;
    }

    return 0;
}

static void apply_patches(uintptr_t base, const runtime_config_t *cfg) {
    checkpoint("[IsaacStaticJoystick] CP6: about to compute addr1/addr2 from base+offset\n");
    uintptr_t addr1 = base + cfg->patch1_offset;
    uintptr_t addr2 = base + cfg->patch2_offset;
    {
        char line[160];
        char b1[24], b2[24];
        u64_to_hex((unsigned long)addr1, b1);
        u64_to_hex((unsigned long)addr2, b2);
        my_strcpy(line, "[IsaacStaticJoystick] CP6-done: addr1=0x");
        my_strcat(line, b1);
        my_strcat(line, " addr2=0x");
        my_strcat(line, b2);
        my_strcat(line, "\n");
        checkpoint(line);
    }

    checkpoint("[IsaacStaticJoystick] CP7: about to read current1/current2 from memory\n");
    uint32_t current1 = *(volatile uint32_t *)addr1;
    uint32_t current2 = *(volatile uint32_t *)addr2;
    {
        char line[160];
        char v1[24], v2[24];
        u64_to_hex((unsigned long)current1, v1);
        u64_to_hex((unsigned long)current2, v2);
        my_strcpy(line, "[IsaacStaticJoystick] CP7-done: current1=0x");
        my_strcat(line, v1);
        my_strcat(line, " current2=0x");
        my_strcat(line, v2);
        my_strcat(line, "\n");
        checkpoint(line);
    }

    int ok1 = (current1 == cfg->patch1_original);
    int ok2 = (current2 == cfg->patch2_original);

    if (!ok1) checkpoint("[IsaacStaticJoystick] patch 1 original bytes mismatch\n");
    else checkpoint("[IsaacStaticJoystick] patch 1 verified\n");
    if (!ok2) checkpoint("[IsaacStaticJoystick] patch 2 original bytes mismatch\n");
    else checkpoint("[IsaacStaticJoystick] patch 2 verified\n");

    if (!ok1 || !ok2) {
        checkpoint("[IsaacStaticJoystick] aborting patch (safety check failed)\n");
        return;
    }

    checkpoint("[IsaacStaticJoystick] no __TEXT memory writes; arming ARM64 hardware breakpoints\n");
    if (arm_hardware_breakpoints(addr1,
                                 cfg->patch1_original, cfg->patch1_new,
                                 addr2,
                                 cfg->patch2_original, cfg->patch2_new) != 0) {
        checkpoint("[IsaacStaticJoystick] hardware breakpoint setup failed; module inert\n");
        return;
    }

    checkpoint("[IsaacStaticJoystick] hardware breakpoints armed (JIT-less; no memory patching)\n");
    checkpoint("[IsaacStaticJoystick] static joystick patch active\n");
}

/* Reintento simple y acotado, igual que la variante base. */
static void try_apply_with_retries(const runtime_config_t *cfg) {
    const int MAX_ATTEMPTS = 5;
    const useconds_t RETRY_DELAY_US = 100000;

    for (int attempt = 0; attempt < MAX_ATTEMPTS; attempt++) {
        uintptr_t base = 0;
        intptr_t slide = 0;
        (void)slide;

        if (find_target_image(cfg->target_uuid, &base, &slide)) {
            checkpoint("[IsaacStaticJoystick] proceeding to apply_patches()\n");
            apply_patches(base, cfg);
            return;
        }

        if (attempt == 0) {
            log_msg("[IsaacStaticJoystick] searching for target image\n");
        }

        usleep(RETRY_DELAY_US);
    }

    log_msg("[IsaacStaticJoystick] UUID not found\n");
}

/* ============================================================
 *  Punto de entrada del dylib
 * ============================================================ */

static char g_config_buf[CONFIG_MAX_SIZE];

/* Cuerpo real del constructor. Usa 'return;' para salidas tempranas;
 * el propio __attribute__((constructor)) que llama a esta funcion se
 * encarga de volcar el reporte a disco pase lo que pase (ver abajo). */
static void isaac_static_joystick_run(void) {
    log_msg("[IsaacStaticJoystick] loaded (runtime-config variant)\n");

    long n = read_config_file(g_config_buf, sizeof(g_config_buf));
    if (n <= 0) {
        log_msg("[IsaacStaticJoystick] config file not found or empty; module inert\n");
        return;
    }

    runtime_config_t cfg;
    /* zero-init manual (sin memset del sistema) */
    {
        char *p = (char *)&cfg;
        for (size_t k = 0; k < sizeof(cfg); k++) p[k] = 0;
    }

    parse_config_buffer(g_config_buf, (size_t)n, &cfg);

    if (!config_is_complete(&cfg)) {
        log_msg("[IsaacStaticJoystick] config incomplete; module inert\n");
        return;
    }

    log_msg("[IsaacStaticJoystick] config loaded; searching for target image\n");
    try_apply_with_retries(&cfg);
}

/* Constructor real: llama al cuerpo de arriba y SIEMPRE vuelca el
 * reporte acumulado a disco al final, sin importar por cual 'return'
 * haya salido isaac_static_joystick_run(). Asi el archivo de reporte
 * aparece siempre que el dylib se haya cargado, incluso si aborta
 * temprano (config ausente, UUID no encontrado, verificacion fallida,
 * etc.) — eso es justamente lo que queremos poder ver desde Files. */
__attribute__((constructor))
static void isaac_static_joystick_ctor(void) {
    isaac_static_joystick_run();
    report_append("[IsaacStaticJoystick] end of report\n");
    flush_report_to_disk();
}
