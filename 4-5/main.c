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

#define MAX_TEXT 128

// предложение (валентинка) от поклонника
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

static int gN = 0;             // количество поклонников
static Offer *gOffers = NULL;  // предложения поклонников (по индексу id)
static Reply *gReplies = NULL; // ответы студентки (по индексу id)

// Флаги (активное ожидание)
static atomic_int *gSubmitted = NULL;  // submitted[i] == 1, когда поклонник i отправил предложение
static atomic_int *gReplied   = NULL;  // replied[i]   == 1, когда студентка выдала ответ поклоннику i

static atomic_int gAllChosen = 0;      // студентка завершила выбор
static atomic_int gWinnerId = -1;      // победитель
static atomic_int gBestScore = -1;     // лучший score

// Мьютекс только для печати, чтобы строки не перемешивались
static pthread_mutex_t gPrintLock = PTHREAD_MUTEX_INITIALIZER;

static void die_pthread(int rc, const char *where) {
    // обработка ошибок pthread-ов
    if (rc == 0) return;
    fprintf(stderr, "pthread error at %s: %s\n", where, strerror(rc));
    exit(1);
}

static void die_errno(const char *where) {
    // обработка системных ошибок (errno)
    fprintf(stderr, "error at %s: %s\n", where, strerror(errno));
    exit(1);
}

static int rand_between(unsigned *seed, int lo, int hi) { // включительно
    // rand_r в диапазоне [lo..hi]
    if (hi < lo) { int t = lo; lo = hi; hi = t; }
    int span = hi - lo + 1;
    return lo + (int)(rand_r(seed) % (unsigned)span);
}

static void safe_print(const char *fmt, ...) {
    // потокобезопасная печать
    va_list ap;
    int rc = pthread_mutex_lock(&gPrintLock);
    die_pthread(rc, "pthread_mutex_lock(print)");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
    rc = pthread_mutex_unlock(&gPrintLock);
    die_pthread(rc, "pthread_mutex_unlock(print)");
}

// аргументы для потока-поклонника
typedef struct {
    int fan_id;
} FanArgs;

static void *fan_thread(void *arg) {
    FanArgs *a = (FanArgs*)arg;
    int id = a->fan_id;

    // локальный seed для rand_r (чтобы потоки не мешали друг другу)
    unsigned seed = (unsigned)time(NULL) ^ (unsigned)(id * 2654435761u);

    // имитация "обдумывания" предложения
    int think = rand_between(&seed, 1, 3);
    sleep((unsigned)think);

    // формируем предложение
    Offer offer;
    offer.fan_id = id;
    offer.score = rand_between(&seed, 1, 100);

    // набор идей для вечера
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
    int k = rand_between(&seed, 0, (int)(sizeof(ideas)/sizeof(ideas[0]) - 1));
    snprintf(offer.text, sizeof(offer.text), "%s", ideas[k]);

    // "отправка" запроса: записываем в общий массив
    gOffers[id] = offer;

    // отмечаем, что запрос отправлен
    atomic_store(&gSubmitted[id], 1);

    safe_print("[Клиент %02d] Отправил валентинку: score=%d, идея='%s' (думал %dс)\n",
               id, offer.score, offer.text, think);

    // активное ожидание ответа от студентки
    while (atomic_load(&gReplied[id]) == 0) {
        sched_yield();
    }

    // читаем ответ
    Reply rep = gReplies[id];
    if (rep.accepted) {
        safe_print("[Клиент %02d] Получил ответ: Принято! Я выбран! (best_score=%d)\n",
                   id, rep.best_score);
    } else {
        safe_print("[Клиент %02d] Получил ответ: Отказ. Победил клиент %02d (best_score=%d). "
                   "Реакция: '%s'\n",
                   id, rep.winner_id, rep.best_score,
                   (offer.score + 10 < rep.best_score) ? "эх, надо было стараться(" : "обидно, почти выиграл!");
    }

    return NULL;
}

static void *girl_thread(void *arg) {
    (void)arg;

    safe_print("[Сервер] Студентка: жду все валентинки...\n");

    // ждём, пока все поклонники отправят предложения (active wait)
    for (;;) {
        int ready = 1;
        for (int i = 0; i < gN; ++i) {
            if (atomic_load(&gSubmitted[i]) == 0) { ready = 0; break; }
        }
        if (ready) break;
        sched_yield();
    }

    safe_print("[Сервер] Все валентинки получены. Выбираю лучшее предложение...\n");

    // выбор лучшего (максимальный score)
    int best_id = 0;
    int best_score = gOffers[0].score;
    for (int i = 1; i < gN; ++i) {
        if (gOffers[i].score > best_score) {
            best_score = gOffers[i].score;
            best_id = i;
        }
    }

    atomic_store(&gWinnerId, best_id);
    atomic_store(&gBestScore, best_score);

    // задержка для наглядности
    sleep(1);

    safe_print("[Сервер] Выбрано предложение клиента %02d: score=%d, идея='%s'\n",
               best_id, best_score, gOffers[best_id].text);

    // рассылка ответов всем поклонникам
    for (int i = 0; i < gN; ++i) {
        gReplies[i].accepted = (i == best_id) ? 1 : 0;
        gReplies[i].winner_id = best_id;
        gReplies[i].best_score = best_score;

        atomic_store(&gReplied[i], 1);
    }

    atomic_store(&gAllChosen, 1);
    safe_print("[Сервер] Ответы разосланы всем. Завершаю работу.\n");
    return NULL;
}

int main(int argc, char **argv) {
    // N задаём аргументом командной строки
    if (argc != 2) {
        fprintf(stderr, "Usage: %s N\n", argv[0]);
        fprintf(stderr, "  N - number of fans (threads), e.g. 10\n");
        return 1;
    }

    gN = atoi(argv[1]);
    if (gN <= 0 || gN > 1000) {
        fprintf(stderr, "N must be in [1..1000]\n");
        return 1;
    }

    // выделяем память под общие структуры
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
    atomic_init(&gAllChosen, 0);
    atomic_init(&gWinnerId, -1);
    atomic_init(&gBestScore, -1);

    // запускаем серверный поток
    pthread_t server;
    int rc = pthread_create(&server, NULL, girl_thread, NULL);
    die_pthread(rc, "pthread_create(server)");

    // запускаем клиентские потоки
    pthread_t *clients = (pthread_t*)calloc((size_t)gN, sizeof(pthread_t));
    FanArgs *args = (FanArgs*)calloc((size_t)gN, sizeof(FanArgs));
    if (!clients || !args) die_errno("calloc(clients/args)");

    for (int i = 0; i < gN; ++i) {
        args[i].fan_id = i;
        rc = pthread_create(&clients[i], NULL, fan_thread, &args[i]);
        die_pthread(rc, "pthread_create(client)");
    }

    // ждём завершения клиентов
    for (int i = 0; i < gN; ++i) {
        rc = pthread_join(clients[i], NULL);
        die_pthread(rc, "pthread_join(client)");
    }

    // ждём завершения сервера
    rc = pthread_join(server, NULL);
    die_pthread(rc, "pthread_join(server)");

    safe_print("[MAIN] Итог: победил клиент %02d, best_score=%d\n",
               atomic_load(&gWinnerId), atomic_load(&gBestScore));

    // освобождение памяти
    free(args);
    free(clients);
    free(gReplied);
    free(gSubmitted);
    free(gReplies);
    free(gOffers);

    return 0;
}