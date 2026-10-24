# ГеоТрекер ВС

Система определения географических координат воздушных судов по видео с нескольких
камер терминала Б аэропорта Шереметьево. Приложение принимает RTSP-потоки камер,
находит самолёты детектором YOLO26 (NVIDIA DeepStream 9.0, TensorRT), сопровождает их
трекером, переводит точку касания самолёта с землёй из пикселей в координаты WGS84
по гомографии каждой камеры и раз в секунду отправляет результат по UDP в формате protobuf.

- [Паспорт проекта](project_passport.md)
- [Техническое задание](https://docs.google.com/document/d/1ofnDw1J2EeLFSGgIVBBEvvBLIBjM1cyu/edit?usp=sharing&ouid=110476095463723163508&rtpof=true&sd=true)

## Документация

| Документ | О чём |
| :--- | :--- |
| [docs/architecture.md](docs/architecture.md) | Конвейер GStreamer/DeepStream, модули исходников, поток данных |
| [docs/configuration.md](docs/configuration.md) | Справочник по `configs/app.ini` и остальным конфигам |
| [docs/model.md](docs/model.md) | Модель YOLO26: экспорт в ONNX, сборка парсера, TensorRT engine |
| [docs/calibration.md](docs/calibration.md) | Калибровка камер: формат CSV, как считается гомография, проверка |
| [docs/output-format.md](docs/output-format.md) | Формат UDP-сообщений (protobuf), пакетная отправка, приёмник |
| [docs/deploy.md](docs/deploy.md) | Развёртывание и эксплуатация |
| [docs/monitoring.md](docs/monitoring.md) | Журнал, события камер, метрики Prometheus |

## Быстрый старт

Приложение собирается и работает только внутри Docker-контейнера с DeepStream 9.0.
Нужна видеокарта NVIDIA с драйвером 590.48.01+ и NVIDIA Container Toolkit.

1. Настоящие конфиги в git не хранятся, в репозитории лежат шаблоны `*.example`.
   Скопируйте их и подставьте свои значения ([справочник](docs/configuration.md)):

   ```
   cp configs/app.ini.example configs/app.ini
   cp configs/config_infer_primary_yolo.txt.example configs/config_infer_primary_yolo.txt
   cp configs/warper.toml.example configs/warper.toml
   cp configs/calibration/camera.csv.example configs/calibration/camera0.csv   # и так для каждой камеры
   ```

2. Экспортируйте модель `models/yolo26m.pt` в ONNX скриптом `models/export_yolo26.py`
   ([docs/model.md](docs/model.md)).

3. Запустите.

   На сервере — через Docker Compose ([docs/deploy.md](docs/deploy.md)):

   ```
   cp .env.example .env
   docker compose up -d --build
   docker compose logs -f
   ```

   При разработке — внутри контейнера DeepStream, где собран парсер DeepStream-Yolo:

   ```
   make
   make engine          # TensorRT engine из ONNX: первый раз несколько минут
   ./build/sheremetevo-app configs/app.ini
   ```

   `make engine` пересобирает engine, только если поменялись ONNX, GPU или параметры
   ([model.md](docs/model.md#3-tensorrt-engine)); в Docker это делается при старте.

Все настройки — в `configs/app.ini` ([справочник](docs/configuration.md)): камеры,
получатель UDP, видеовыход, трекер, переподключение RTSP, пороги.
При старте программа печатает ошибку репроекции калибровки по каждой камере
(`[CALIB] ...`), затем раз в 5 минут — средние fps и число объектов по камерам (`[STATS] ...`).
Журнал и метрики Prometheus настраиваются в `[logging]` и `[metrics]`
([monitoring.md](docs/monitoring.md)).

Остановка — `Ctrl+C`, `SIGTERM` или `docker compose down`: приложение отправляет EOS и
корректно закрывает выходные потоки. Повторный сигнал завершает процесс сразу.

## Тесты

Юнит-тесты не требуют GPU и DeepStream: им нужны только glib, OpenCV и protobuf,
поэтому их можно запускать в контейнере DeepStream, в CI или на любой машине с этими
пакетами (`libglib2.0-dev libopencv-dev libprotobuf-dev protobuf-compiler`).

```
make test                     # собрать и запустить все тесты
make test TEST_FILTER=csv     # только тесты, в имени которых есть подстрока
```

Покрыты: гомография пиксель → WGS84 (`pixel_transformer`: точность, RANSAC в метрах,
ошибка репроекции), разбор `app.ini` и CSV калибровки (`app_config`), сборка
protobuf-сообщений и разбиение на датаграммы (`points_sender`, отправка на 127.0.0.1).
Тесты лежат в `tests/`, каркас — `tests/test.hpp` (без внешних библиотек).

## Структура репозитория

```
*.cpp, *.hpp           исходники приложения (см. docs/architecture.md)
Makefile               сборка: make, make run, make test, make gen-graph, make graph, make clean
configs/               конфиги приложения, nvinfer, nvdewarper, калибровки (шаблоны *.example)
models/                веса YOLO26 (.pt), скрипт экспорта export_yolo26.py;
                       ONNX, labels.txt и engine создаются локально и в git не хранятся
build/                 результат сборки (в git не хранится)
graphs/                графы конвейера для отладки (make gen-graph, в git не хранится)
proto/points.proto     схема UDP-сообщений (protobuf), см. docs/output-format.md
tools/udp_receiver.py  отладочный приёмник UDP-сообщений
tests/                 юнит-тесты (make test)
docker/, docker-compose.yml, .env.example
                       сборка образа и запуск в Docker (см. docs/deploy.md)
docs/                  документация
```
