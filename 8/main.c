#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <time.h>
#include <sched.h>
#include <stdarg.h>
#include <signal.h>

#define MAX_TEXT 128


// "валентинка" (предложение поклонника)
typedef struct {
    int fan_id;
    int score;                 // "привлекательность" предложения
    char text[MAX_TEXT];       // описание вечера
} Offer;

// ответ студентки каждому поклоннику
typedef struct {
    int accepted;              // 1 = да, 0 = нет
    int winner_id;             // кто победил (для общего сведения)
    int best_score;            // лучший балл (для общего сведения)
} Reply;


static int gN = 0;             // количество поклонников (кол-во клиентских потоков)
static Offer *gOffers = NULL;  // массив предложений: gOffers[i] — предложение i-го поклонника
static Reply *gReplies = NULL; // массив ответов: gReplies[i] — ответ i-му поклоннику

// Флаги (активное ожидание)
// submitted[i] == 1, когда поклонник i отправил предложение
// replied[i]   == 1, когда студентка выдала ответ поклоннику i
static atomic_int *gSubmitted = NULL;
static atomic_int *gReplied   = NULL;

// Итоговые данные (для печати результата в main)
static atomic_int gWinnerId   = -1;
static atomic_int gBestScore  = -1;

// Флаг запроса на корректное завершение (SIGINT)
// Если пользователь нажал Ctrl+C — выставляем gStop=1 и завершаемся корректно
static atomic_int gStop       = 0;

// Печать синхронизируем мьютексом, чтобы строки не перемешивались
static pthread_mutex_t gPrintLock = PTHREAD_MUTEX_INITIALIZER;

// Лог-файл (8 баллов): дублируем вывод в файл
static FILE *gLogFile = NULL;


static void die_pthread(int rc, const char *where) {
    // единая точка выхода при ошибках pthread-ов
    if (rc == 0) return;
    fprintf(stderr, "pthread error at %s: %s\n", where, strerror(rc));
    exit(1);
}

static void die_errno(const char *where) {
    // единая точка выхода при ошибках системных вызовов/stdio
    fprintf(stderr, "error at %s: %s\n", where, strerror(errno));
    exit(1);
}

static void safe_print(const char *fmt, ...) {
    va_list ap;

    // лочим, чтобы вывод разных потоков не смешивался
    int rc = pthread_mutex_lock(&gPrintLock);
    die_pthread(rc, "pthread_mutex_lock(print)");

    // печать в консоль
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);

    // печать в файл (если задан ключ -o)
    if (gLogFile) {
        va_start(ap, fmt);
        vfprintf(gLogFile, fmt, ap);
        va_end(ap);
        fflush(gLogFile);
    }

    rc = pthread_mutex_unlock(&gPrintLock);
    die_pthread(rc, "pthread_mutex_unlock(print)");
}


// генерация в диапазоне [lo..hi], используем rand_r (thread-safe по seed)
static int rand_between(unsigned *seed, int lo, int hi) { // включительно
    if (hi < lo) { int t = lo; lo = hi; hi = t; }
    int span = hi - lo + 1;
    return lo + (int)(rand_r(seed) % (unsigned)span);
}


// обработчик Ctrl+C
static void on_sigint(int signo) {
    (void)signo;
    atomic_store(&gStop, 1);
}

typedef struct {
    int fan_id;        // номер поклонника (индекс в массивах)
    unsigned base_seed;// базовый seed, чтобы сценарий был воспроизводим при заданном SEED
} FanArgs;

static void *fan_thread(void *arg) {
    FanArgs *a = (FanArgs*)arg;
    int id = a->fan_id;

    // локальный seed для rand_r: общий base_seed
    unsigned seed = a->base_seed ^ (unsigned)(id * 2654435761u);

    // "думает" над предложением (имитация параллельного поведения)
    int think = rand_between(&seed, 1, 3);
    for (int s = 0; s < think; ++s) {
        // если нажали Ctrl+C — корректно выходим
        if (atomic_load(&gStop)) {
            safe_print("[Клиент %02d] Прервано (SIGINT) во время обдумывания.\n", id);
            return NULL;
        }
        sleep(1u);
    }

    // формируем предложение
    Offer offer;
    offer.fan_id = id;
    offer.score = rand_between(&seed, 1, 100);

    // идеи вечера — фиксированный набор, выбираем случайно
    const char *ideas[] = {
        "прогулка по городу + кофе",
        "кино + пицца",
        "ужин при свечах",
        "каток + горячий шоколад",
        "настолки + чай",
        "пикник (если погода позволит)",
        "музей + прогулка",
        "концерт + поздний ужин"
    };
    const int k = rand_between(&seed, 0, (int)(sizeof(ideas)/sizeof(ideas[0]) - 1));
    snprintf(offer.text, sizeof(offer.text), "%s", ideas[k]);

    // отправка запроса "на сервер":
    // кладём предложение в свой слот и отмечаем флаг отправки
    gOffers[id] = offer;
    atomic_store(&gSubmitted[id], 1);

    safe_print("[Клиент %02d] Отправил валентинку: score=%d, идея='%s' (думал %dс)\n",
               id, offer.score, offer.text, think);

    // активное ожидание ответа:
    // по условию поклонник получает ответ только после того, как все отправили предложения
    while (!atomic_load(&gReplied[id])) {
        if (atomic_load(&gStop)) {
            safe_print("[Клиент %02d] Прервано (SIGINT) во время ожидания ответа.\n", id);
            return NULL;
        }
        // отдаём квант процессора, чтобы не "жечь" CPU полностью
        sched_yield();
    }

    // получаем ответ (студентка заполнила gReplies[id])
    Reply rep = gReplies[id];

    // предметная реакция клиента
    if (rep.accepted) {
        safe_print("[Клиент %02d] Ответ: Принято! (best_score=%d)\n", id, rep.best_score);
    } else {
        // если winner_id < 0 — значит завершение по SIGINT
        if (rep.winner_id < 0) {
            safe_print("[Клиент %02d] Ответ: Отказ. (работа остановлена пользователем)\n", id);
        } else {
            safe_print("[Клиент %02d] Ответ: Отказ. Победил %02d (best_score=%d). Реакция: '%s'\n",
                       id, rep.winner_id, rep.best_score,
                       (offer.score + 10 < rep.best_score) ? "надо было стараться(" : "обидно, почти выиграл!");
        }
    }

    return NULL;
}

// если работа прервана, всем выдаём отказ и помечаем, что ответ готов
static void send_abort_replies(void) {
    for (int i = 0; i < gN; ++i) {
        gReplies[i].accepted = 0;
        gReplies[i].winner_id = -1;
        gReplies[i].best_score = -1;
        atomic_store(&gReplied[i], 1);
    }
}


static void *girl_thread(void *arg) {
    (void)arg;

    safe_print("[Сервер] Студентка: жду все валентинки...\n");

    // ждём, пока все N клиентов выставят submitted[i] (активно)
    for (;;) {
        // если прервали по Ctrl+C — сразу рассылаем отказ и выходим
        if (atomic_load(&gStop)) {
            safe_print("[Сервер] Получен SIGINT. Рассылаю всем отказ и завершаю.\n");
            send_abort_replies();
            return NULL;
        }

        int ready = 1;
        for (int i = 0; i < gN; ++i) {
            if (!atomic_load(&gSubmitted[i])) { ready = 0; break; }
        }
        if (ready) break;

        sched_yield();
    }

    safe_print("[Сервер] Все валентинки получены. Выбираю лучшее предложение...\n");

    // выбираем предложение с максимальным score
    int best_id = 0;
    int best_score = gOffers[0].score;
    for (int i = 1; i < gN; ++i) {
        if (gOffers[i].score > best_score) {
            best_score = gOffers[i].score;
            best_id = i;
        }
    }

    // сохраняем итог для main
    atomic_store(&gWinnerId, best_id);
    atomic_store(&gBestScore, best_score);

    // имитация времени выбора
    for (int s = 0; s < 1; ++s) {
        if (atomic_load(&gStop)) {
            safe_print("[Сервер] SIGINT во время выбора. Рассылаю отказ и завершаю.\n");
            send_abort_replies();
            return NULL;
        }
        sleep(1u);
    }

    safe_print("[Сервер] Выбрано предложение клиента %02d: score=%d, идея='%s'\n",
               best_id, best_score, gOffers[best_id].text);

    // рассылка ответов всем клиентам
    for (int i = 0; i < gN; ++i) {
        gReplies[i].accepted = (i == best_id) ? 1 : 0;
        gReplies[i].winner_id = best_id;
        gReplies[i].best_score = best_score;
        atomic_store(&gReplied[i], 1);
    }

    safe_print("[Сервер] Ответы разосланы всем. Завершаю работу.\n");
    return NULL;
}

// безопасный парс int (проверка хвоста строки, диапазона)
static int parse_int(const char *s, int *out) {
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (!s || !s[0] || (end && *end)) return 0;
    if (v < -2147483647L || v > 2147483647L) return 0;
    *out = (int)v;
    return 1;
}

// безопасный парс unsigned (для SEED)
static int parse_uint(const char *s, unsigned *out) {
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    if (!s || !s[0] || (end && *end)) return 0;
    if (v > 0xFFFFFFFFul) return 0;
    *out = (unsigned)v;
    return 1;
}

// простая "конфигурация": строки вида N=10, SEED=12345
// считываем построчно и обновляем параметры, если нашли подходящую строку
static void read_config(const char *fname, int *outN, unsigned *outSeed) {
    FILE *f = fopen(fname, "r");
    if (!f) die_errno("fopen(config)");

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        // убираем перевод строки
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }

        int n_tmp = 0;
        unsigned s_tmp = 0;

        if (sscanf(line, "N=%d", &n_tmp) == 1) {
            *outN = n_tmp;
            continue;
        }
        if (sscanf(line, "SEED=%u", &s_tmp) == 1) {
            *outSeed = s_tmp;
            continue;
        }
    }

    fclose(f);
}


int main(int argc, char **argv) {
    /*
     * - вывод в файл (и в консоль одновременно): -o <file>
     * - альтернативный ввод из конфигурационного файла: -c <file>
     * - ввод из командной строки: -n <N> -s <SEED>
     *
     * Если задан -c, то значения берутся из файла, а -n/-s игнорируются.
     */
    int n_from_cli = -1;
    unsigned seed_from_cli = (unsigned)time(NULL);

    const char *out_name = NULL; // имя лог-файла (если нужно)
    const char *cfg_name = NULL; // имя конфиг-файла (если нужно)

    // разбор ключей командной строки
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-n")) {
            // количество потоков-клиентов
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value for -n\n");
                return 1;
            }
            if (!parse_int(argv[++i], &n_from_cli)) {
                fprintf(stderr, "Invalid value for -n\n");
                return 1;
            }
        } else if (!strcmp(argv[i], "-s")) {
            // seed для воспроизводимости
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value for -s\n");
                return 1;
            }
            if (!parse_uint(argv[++i], &seed_from_cli)) {
                fprintf(stderr, "Invalid value for -s\n");
                return 1;
            }
        } else if (!strcmp(argv[i], "-o")) {
            // файл вывода
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value for -o\n");
                return 1;
            }
            out_name = argv[++i];
        } else if (!strcmp(argv[i], "-c")) {
            // конфигурационный файл
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value for -c\n");
                return 1;
            }
            cfg_name = argv[++i];
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            // справка
            fprintf(stderr,
                    "Usage:\n"
                    "  %s -n N [-s SEED] [-o OUT]\n"
                    "  %s -c CONFIG [-o OUT]\n"
                    "\n"
                    "  -n N      number of fans (1..1000)\n"
                    "  -s SEED   optional seed\n"
                    "  -c FILE   read N and SEED from config file (N=..., SEED=...)\n"
                    "  -o FILE   write log to file (in addition to console)\n",
                    argv[0], argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            fprintf(stderr, "Use -h for help\n");
            return 1;
        }
    }

    // параметры по умолчанию
    int n = n_from_cli;
    unsigned base_seed = seed_from_cli;

    // если указан конфиг — берём параметры из файла
    // (при этом ключи -n/-s считаются неактуальными)
    if (cfg_name) {
        read_config(cfg_name, &n, &base_seed);
    }

    // проверка диапазона
    if (n < 1 || n > 1000) {
        fprintf(stderr, "N must be in [1..1000]\n");
        return 1;
    }

    gN = n;

    // лог-файл (если задан)
    if (out_name) {
        gLogFile = fopen(out_name, "w");
        if (!gLogFile) die_errno("fopen(output)");
    }

    // настройка SIGINT
    // цель: корректно завершиться по Ctrl+C (без зависаний потоков)
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) != 0) die_errno("sigaction(SIGINT)");

    // выделяем общую память под предложения/ответы/флаги
    gOffers = (Offer*)calloc((size_t)gN, sizeof(Offer));
    gReplies = (Reply*)calloc((size_t)gN, sizeof(Reply));
    gSubmitted = (atomic_int*)calloc((size_t)gN, sizeof(atomic_int));
    gReplied = (atomic_int*)calloc((size_t)gN, sizeof(atomic_int));
    if (!gOffers || !gReplies || !gSubmitted || !gReplied) die_errno("calloc");

    // инициализация атомарных флагов
    for (int i = 0; i < gN; ++i) {
        atomic_init(&gSubmitted[i], 0);
        atomic_init(&gReplied[i], 0);
    }
    atomic_init(&gWinnerId, -1);
    atomic_init(&gBestScore, -1);
    atomic_init(&gStop, 0);

    safe_print("[MAIN] Старт: N=%d, SEED=%u (Ctrl+C для прерывания)\n", gN, base_seed);

    // создаём поток сервера (студентка)
    pthread_t server;
    int rc = pthread_create(&server, NULL, girl_thread, NULL);
    die_pthread(rc, "pthread_create(server)");

    // создаём N потоков клиентов (поклонники)
    pthread_t *clients = (pthread_t*)calloc((size_t)gN, sizeof(pthread_t));
    FanArgs *args = (FanArgs*)calloc((size_t)gN, sizeof(FanArgs));
    if (!clients || !args) die_errno("calloc(clients/args)");

    for (int i = 0; i < gN; ++i) {
        args[i].fan_id = i;
        args[i].base_seed = base_seed;
        rc = pthread_create(&clients[i], NULL, fan_thread, &args[i]);
        die_pthread(rc, "pthread_create(client)");
    }

    // ждём завершения всех клиентов
    for (int i = 0; i < gN; ++i) {
        rc = pthread_join(clients[i], NULL);
        die_pthread(rc, "pthread_join(client)");
    }

    // ждём завершения сервера
    rc = pthread_join(server, NULL);
    die_pthread(rc, "pthread_join(server)");

    // печать итогов
    int win = atomic_load(&gWinnerId);
    int best = atomic_load(&gBestScore);

    if (atomic_load(&gStop)) {
        safe_print("[MAIN] Завершение по SIGINT.\n");
    } else {
        safe_print("[MAIN] Итог: победил клиент %02d, best_score=%d\n", win, best);
    }

    // освобождение ресурсов
    free(args);
    free(clients);
    free(gReplied);
    free(gSubmitted);
    free(gReplies);
    free(gOffers);

    if (gLogFile) fclose(gLogFile);

    return 0;
}