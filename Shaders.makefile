GLSLANG = glslangValidator
VULKAN_VER = 1.2

RTX_SRC_DIR = src/engine/renderer/shaders/rtx
COMP_SRC_DIR = src/engine/renderer/shaders/compute
GLSL_SRC_DIR = src/engine/renderer/shaders/glsl
POST_SRC_DIR = src/engine/renderer/shaders/postprocess

RTX_OUT_DIR = baseq3/shaders/rtx
COMP_OUT_DIR = baseq3/shaders/compute
GLSL_OUT_DIR = baseq3/shaders/glsl
POST_OUT_DIR = baseq3/shaders/postprocess

# RTX shaders
RTX_SPVS = $(patsubst $(RTX_SRC_DIR)/%.rgen,$(RTX_OUT_DIR)/%.spv,$(wildcard $(RTX_SRC_DIR)/*.rgen)) \
           $(patsubst $(RTX_SRC_DIR)/%.rchit,$(RTX_OUT_DIR)/%.spv,$(wildcard $(RTX_SRC_DIR)/*.rchit)) \
           $(patsubst $(RTX_SRC_DIR)/%.rmiss,$(RTX_OUT_DIR)/%.spv,$(wildcard $(RTX_SRC_DIR)/*.rmiss)) \
           $(patsubst $(RTX_SRC_DIR)/%.rahit,$(RTX_OUT_DIR)/%.spv,$(wildcard $(RTX_SRC_DIR)/*.rahit))

# Compute shaders
COMP_SPVS = $(patsubst $(COMP_SRC_DIR)/%.comp,$(COMP_OUT_DIR)/%.spv,$(wildcard $(COMP_SRC_DIR)/*.comp))

# GLSL shaders - producing both name.spv and name_stage.spv to be safe
GLSL_VERT_SPVS = $(patsubst $(GLSL_SRC_DIR)/%.vert,$(GLSL_OUT_DIR)/%_vert.spv,$(wildcard $(GLSL_SRC_DIR)/*.vert)) \
                 $(patsubst $(GLSL_SRC_DIR)/%.vert,$(GLSL_OUT_DIR)/%.spv,$(wildcard $(GLSL_SRC_DIR)/*.vert))
GLSL_FRAG_SPVS = $(patsubst $(GLSL_SRC_DIR)/%.frag,$(GLSL_OUT_DIR)/%_frag.spv,$(wildcard $(GLSL_SRC_DIR)/*.frag)) \
                 $(patsubst $(GLSL_SRC_DIR)/%.frag,$(GLSL_OUT_DIR)/%.spv,$(wildcard $(GLSL_SRC_DIR)/*.frag))

# Postprocess shaders
POST_VERT_SPVS = $(patsubst $(POST_SRC_DIR)/%.vert,$(POST_OUT_DIR)/%_vert.spv,$(wildcard $(POST_SRC_DIR)/*.vert)) \
                 $(patsubst $(POST_SRC_DIR)/%.vert,$(POST_OUT_DIR)/%.spv,$(wildcard $(POST_SRC_DIR)/*.vert))
POST_FRAG_SPVS = $(patsubst $(POST_SRC_DIR)/%.frag,$(POST_OUT_DIR)/%_frag.spv,$(wildcard $(POST_SRC_DIR)/*.frag)) \
                 $(patsubst $(POST_SRC_DIR)/%.frag,$(POST_OUT_DIR)/%.spv,$(wildcard $(POST_SRC_DIR)/*.frag))

ALL_SPVS = $(RTX_SPVS) $(COMP_SPVS) $(GLSL_VERT_SPVS) $(GLSL_FRAG_SPVS) $(POST_VERT_SPVS) $(POST_FRAG_SPVS)

.PHONY: all clean

all: $(ALL_SPVS)

# Also copy uber shaders to baseq3/shaders/ as expected by some code
baseq3/shaders/uber.vert.spv: $(GLSL_SRC_DIR)/uber.vert
	@mkdir -p baseq3/shaders
	$(GLSLANG) -V --target-env vulkan$(VULKAN_VER) -o $@ $<

baseq3/shaders/uber.frag.spv: $(GLSL_SRC_DIR)/uber.frag
	@mkdir -p baseq3/shaders
	$(GLSLANG) -V --target-env vulkan$(VULKAN_VER) -o $@ $<

all: baseq3/shaders/uber.vert.spv baseq3/shaders/uber.frag.spv

$(RTX_OUT_DIR)/%.spv: $(RTX_SRC_DIR)/%.rgen
	@mkdir -p $(RTX_OUT_DIR)
	$(GLSLANG) -V --target-env vulkan$(VULKAN_VER) -o $@ $<

$(RTX_OUT_DIR)/%.spv: $(RTX_SRC_DIR)/%.rchit
	@mkdir -p $(RTX_OUT_DIR)
	$(GLSLANG) -V --target-env vulkan$(VULKAN_VER) -o $@ $<

$(RTX_OUT_DIR)/%.spv: $(RTX_SRC_DIR)/%.rmiss
	@mkdir -p $(RTX_OUT_DIR)
	$(GLSLANG) -V --target-env vulkan$(VULKAN_VER) -o $@ $<

$(RTX_OUT_DIR)/%.spv: $(RTX_SRC_DIR)/%.rahit
	@mkdir -p $(RTX_OUT_DIR)
	$(GLSLANG) -V --target-env vulkan$(VULKAN_VER) -o $@ $<

$(COMP_OUT_DIR)/%.spv: $(COMP_SRC_DIR)/%.comp
	@mkdir -p $(COMP_OUT_DIR)
	$(GLSLANG) -V --target-env vulkan$(VULKAN_VER) -o $@ $<

# Rules for GLSL
$(GLSL_OUT_DIR)/%_vert.spv: $(GLSL_SRC_DIR)/%.vert
	@mkdir -p $(GLSL_OUT_DIR)
	$(GLSLANG) -V --target-env vulkan$(VULKAN_VER) -o $@ $<

$(GLSL_OUT_DIR)/%_frag.spv: $(GLSL_SRC_DIR)/%.frag
	@mkdir -p $(GLSL_OUT_DIR)
	$(GLSLANG) -V --target-env vulkan$(VULKAN_VER) -o $@ $<

$(GLSL_OUT_DIR)/%.spv: $(GLSL_SRC_DIR)/%.vert
	@mkdir -p $(GLSL_OUT_DIR)
	$(GLSLANG) -V --target-env vulkan$(VULKAN_VER) -o $@ $<

$(GLSL_OUT_DIR)/%.spv: $(GLSL_SRC_DIR)/%.frag
	@mkdir -p $(GLSL_OUT_DIR)
	$(GLSLANG) -V --target-env vulkan$(VULKAN_VER) -o $@ $<

# Rules for Postprocess
$(POST_OUT_DIR)/%_vert.spv: $(POST_SRC_DIR)/%.vert
	@mkdir -p $(POST_OUT_DIR)
	$(GLSLANG) -V --target-env vulkan$(VULKAN_VER) -o $@ $<

$(POST_OUT_DIR)/%_frag.spv: $(POST_SRC_DIR)/%.frag
	@mkdir -p $(POST_OUT_DIR)
	$(GLSLANG) -V --target-env vulkan$(VULKAN_VER) -o $@ $<

$(POST_OUT_DIR)/%.spv: $(POST_SRC_DIR)/%.vert
	@mkdir -p $(POST_OUT_DIR)
	$(GLSLANG) -V --target-env vulkan$(VULKAN_VER) -o $@ $<

$(POST_OUT_DIR)/%.spv: $(POST_SRC_DIR)/%.frag
	@mkdir -p $(POST_OUT_DIR)
	$(GLSLANG) -V --target-env vulkan$(VULKAN_VER) -o $@ $<

clean:
	rm -f $(ALL_SPVS) baseq3/shaders/uber.vert.spv baseq3/shaders/uber.frag.spv
