# Журнал и метрики

Приложение пишет журнал (stdout/stderr и, если нужно, файл) и может отдавать метрики
в формате Prometheus. Внешних сервисов для этого не нужно: журнал собирает Docker,
метрики можно смотреть через `curl` или подключить к Prometheus/Grafana позже.
Настройки — секции `[logging]` и `[metrics]` в `app.ini`
([configuration.md](configuration.md)).

## Журнал

### Формат

Каждая запись — одна строка: время с часовым поясом, уровень, компонент и номер
камеры, если запись про конкретную камеру.

`format=text` (по умолчанию):

```
2026-10-21T17:31:45.123+03:00 INFO  [CALIB] camera 0: inliers 18/20, reprojection error median 2.1 m, p90 4.0 m, max 6.3 m
2026-10-21T17:31:50.002+03:00 INFO  [STATS] camera=0 25.0 fps, 1.3 objects/frame
2026-10-21T17:32:10.517+03:00 ERROR [CAMERA] camera=2 camera lost: no frames for 10 s, reconnecting in 5 s
2026-10-21T17:32:15.520+03:00 WARN  [RTSP] camera=2 Could not open resource for reading, reconnecting in 5 s (attempt 2, down for 5 s)
2026-10-21T17:33:02.004+03:00 INFO  [CAMERA] camera=2 camera restored after 52 s down, 3 reconnect attempt(s)
```

`format=json` — то же самое, по JSON-объекту на строку (для Loki, ELK, `jq`):

```json
{"ts":"2026-10-21T17:32:10.517+03:00","level":"error","component":"CAMERA","camera":2,"msg":"camera lost: no frames for 10 s, reconnecting in 5 s"}
```

Поля `component` и `camera` есть не у всех записей. Многострочные сообщения
(например, ошибки GStreamer с отладочной информацией) склеиваются в одну строку
через ` | `.

Уровни: `debug`, `info`, `warn`, `error`. `debug` и `info` выводятся в stdout,
`warn` и `error` — в stderr. Часовой пояс задаёт переменная `TZ`
(в `docker-compose.yml` — `Europe/Moscow`).

### Компоненты

| Компонент | Что пишет |
| :--- | :--- |
| `CAMERA` | Пропажа и восстановление камеры (см. ниже) |
| `RTSP` | Повторные неудачные попытки подключения, EOS от сервера камеры |
| `STATS` | Раз в 5 с: fps и число объектов по каждой камере; `warn`, если кадров нет |
| `CALIB` | Качество калибровки каждой камеры при старте |
| `HEALTH` | Heartbeat-файл, перезапуск из-за отсутствия кадров, зависание главного цикла |
| `METRICS` | Адрес эндпоинта метрик, ошибка открытия порта |
| `UDP` | Ошибки отправки и переполнение очереди отправки точек |
| `GST` | Сообщения шины GStreamer: ошибки, предупреждения, смена состояния конвейера |
| `PROBE` | Кадр без калибровки; на `debug` — установка пробы |
| `APP`, `CONFIG`, `CALIB`, `ZONE`, `PIPELINE` | Запуск и остановка, ошибки конфига, калибровки и зон, ошибки сборки конвейера |
| без компонента | Сообщения через `g_print`/`g_printerr` из сторонних библиотек |

Собственные логи GStreamer и DeepStream (`GST_DEBUG`, сообщения nvinfer о
TensorRT) идут мимо журнала приложения и форматом не обладают.

### События камер

Камеру, от которой нет кадров дольше `[rtsp] stall-timeout-sec` или которая
прислала ошибку, приложение переподключает само. В журнале это выглядит так:

| Уровень | Запись | Когда |
| :--- | :--- | :--- |
| `error` | `[CAMERA] camera=N camera lost: <причина>` | Первый сбой камеры, которая работала (или не подключилась при старте). Одна запись на простой |
| `warn` | `[RTSP] camera=N <причина> ... (attempt K, down for S s)` | Каждая следующая неудачная попытка |
| `info` | `[CAMERA] camera=N camera restored after S s down, K reconnect attempt(s)` | От камеры снова пошли кадры |

Поиск по журналу:

```
docker compose logs app | grep '\[CAMERA\]'
docker compose logs app --since 1h | grep -E ' (ERROR|WARN) '
# при format=json
docker compose logs --no-log-prefix app | jq -c 'select(.component == "CAMERA")'
```

Если позже понадобятся оповещения (Telegram, почта), их удобнее строить на метриках
(`sheremetevo_camera_up`, см. ниже) или на записях `[CAMERA] ... camera lost` в
системе сбора логов, приложение для этого менять не нужно.

### Файл журнала

В Docker журнал собирает Docker с ротацией 5 × 50 МБ, файл не нужен. При запуске без
Docker можно задать `[logging] file=...`: записи дублируются в файл, при превышении
`max-size-mb` файл переименовывается в `.1`, старые сдвигаются до `.<max-files>`,
самый старый удаляется. Папку для файла нужно создать заранее.

## Метрики Prometheus

Включаются в `app.ini`:

```ini
[metrics]
enabled=true
bind-address=0.0.0.0
port=9464
```

Проверка: `curl http://localhost:9464/metrics`. Значения обновляются раз в секунду.

| Метрика | Тип | Описание |
| :--- | :--- | :--- |
| `sheremetevo_uptime_seconds` | gauge | Время работы приложения |
| `sheremetevo_camera_up{camera}` | gauge | `1` — от камеры идут кадры (последний не старше `stall-timeout-sec`), `0` — нет |
| `sheremetevo_camera_down_seconds{camera}` | gauge | Длительность текущего простоя камеры, `0`, если работает |
| `sheremetevo_camera_last_frame_age_seconds{camera}` | gauge | Сколько секунд назад пришёл последний кадр, `-1` — кадров ещё не было |
| `sheremetevo_camera_restarts_total{camera}` | counter | Перезапуски камеры после ошибки или зависания |
| `sheremetevo_camera_fps{camera}` | gauge | fps на выходе анализа за последние 5 с (то же, что в `[STATS]`) |
| `sheremetevo_camera_frames_total{camera}` | counter | Обработанные кадры |
| `sheremetevo_camera_objects_total{camera}` | counter | Найденные объекты |
| `sheremetevo_camera_latency_seconds_sum{camera}`, `_count` | summary | Задержка обработки: от прихода кадра в `nvstreammux` до выхода из детектора/трекера |
| `sheremetevo_udp_datagrams_sent_total` | counter | Отправленные датаграммы с точками |
| `sheremetevo_udp_send_errors_total` | counter | Ошибки отправки |
| `sheremetevo_udp_frames_dropped_total` | counter | Кадры, отброшенные из-за переполнения очереди отправки |

Задержка считается по `ntp_timestamp` кадра, который `nvstreammux` по умолчанию
ставит по системным часам в момент прихода кадра. Сеть от камеры до сервера и
декодирование в неё не входят.

Полезные выражения PromQL:

```
# камеры, которые сейчас не работают
sheremetevo_camera_up == 0
# средняя задержка обработки за 5 минут, секунды
rate(sheremetevo_camera_latency_seconds_sum[5m]) / rate(sheremetevo_camera_latency_seconds_count[5m])
# fps по счётчику кадров
rate(sheremetevo_camera_frames_total[1m])
# перезапуски камер за час
increase(sheremetevo_camera_restarts_total[1h])
```

### Если появится Prometheus

Минимальный `prometheus.yml` (приложение в сети хоста, порт 9464):

```yaml
scrape_configs:
  - job_name: sheremetevo
    scrape_interval: 15s
    static_configs:
      - targets: ["<сервер>:9464"]
```

Пример правил оповещения для Alertmanager:

```yaml
groups:
  - name: sheremetevo
    rules:
      - alert: CameraDown
        expr: sheremetevo_camera_up == 0
        for: 1m
        annotations:
          summary: "Камера {{ $labels.camera }} не присылает кадры"
      - alert: AppDown
        expr: up{job="sheremetevo"} == 0
        for: 2m
      - alert: UdpSendErrors
        expr: increase(sheremetevo_udp_send_errors_total[5m]) > 0
```
