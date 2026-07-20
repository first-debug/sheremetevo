APP:= sheremetevo-app

BUILD_DIR:= build
CONFIGS_DIR:= configs
GRAPHS_DIR:= graphs

TARGET:= $(BUILD_DIR)/$(APP)

TARGET_DEVICE = $(shell gcc -dumpmachine | cut -f1 -d -)

NVDS_VERSION:=9.0

LIB_INSTALL_DIR?=/opt/nvidia/deepstream/deepstream-$(NVDS_VERSION)/lib/

SRCS:= $(wildcard *.c)

INCS:= $(wildcard *.h)

PKGS:= gstreamer-1.0

OBJS:= $(SRCS:.c=.o)

BUILD_OBJS:= $(addprefix $(BUILD_DIR)/, $(OBJS))

CFLAGS+= $(shell pkg-config --cflags $(PKGS)) \
		 -I/opt/nvidia/deepstream/deepstream/sources/includes \
		 -L/opt/nvidia/deepstream/deepstream/lib -lnvdsgst_meta -lnvds_meta \
		 -L$(BUILD_DIR)

LIBS:= $(shell pkg-config --libs $(PKGS)) \
		-L$(LIB_INSTALL_DIR) -lnvdsgst_meta -lnvds_meta -lnvds_yml_parser \
		-lcuda -lnvbufsurface -Wl,-rpath,$(LIB_INSTALL_DIR)

.PHONY: all run install clean

all: $(TARGET)

run: all
	$(MAKE) run-multi-src

run-single-src: $(TARGET)
	$(TARGET) $(CONFIGS_DIR)/config_infer_primary_yolo.txt \
		rtspt://192.168.10.183:8554/output \
		rtspt://192.168.10.183:8554/svo1

run-multi-src: $(TARGET)
	$(TARGET) $(CONFIGS_DIR)/config_infer_primary_yolo.txt \
		rtspt://192.168.10.183:8554/output \
		rtspt://192.168.10.183:8554/svo1 \
		rtspt://192.168.10.183:8554/svo2 \
		rtspt://192.168.10.183:8554/svo3 \
		rtspt://192.168.10.183:8554/svo4

$(BUILD_DIR):
	mkdir -p $@

$(GRAPHS_DIR):
	mkdir -p $@

$(BUILD_DIR)/%.o: %.c $(INCS) Makefile | $(BUILD_DIR)
	$(CC) -c -o $@ $(CFLAGS) $<

$(TARGET): $(BUILD_OBJS) Makefile | $(BUILD_DIR)
	$(CC) -o $@ $(BUILD_OBJS) $(LIBS)

gen-graph: $(TARGET) | $(GRAPHS_DIR)
	GST_DEBUG_DUMP_DOT_DIR=graphs $(MAKE) run-multi-src

graph:
	dot -Tpng graphs/sheremetevo.dot -o graphs/sheremetevo.png

clean:
	rm -rf $(BUILD_DIR)
	rm -rf $(GRAPHS_DIR)
