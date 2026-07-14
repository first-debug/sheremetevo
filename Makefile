APP:= sheremetevo-app
BUILD_DIR:= build

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
		 -L/opt/nvidia/deepstream/deepstream/lib -lnvdsgst_meta -lnvds_meta

ifeq ($(SAVE_TO), file)
	CFLAGS+= -DSAVE_TO_FILE
endif

LIBS:= $(shell pkg-config --libs $(PKGS)) \
		-L$(LIB_INSTALL_DIR) -lnvdsgst_meta -lnvds_meta -lnvds_yml_parser \
		-lcuda -lnvbufsurface -Wl,-rpath,$(LIB_INSTALL_DIR)

.PHONY: all run install clean

all: $(TARGET)

run: $(TARGET)
	$(TARGET) /opt/nvidia/deepstream/deepstream/samples/configs/deepstream-app/config_infer_primary.txt rtspt://localhost:8554/live

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/%.o: %.c $(INCS) Makefile | $(BUILD_DIR)
	$(CC) -c -o $@ $(CFLAGS) $<

$(TARGET): $(BUILD_OBJS) Makefile | $(BUILD_DIR)
	$(CC) -o $@ $(BUILD_OBJS) $(LIBS)

clean:
	rm -rf $(BUILD_DIR)
