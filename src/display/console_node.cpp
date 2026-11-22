#include "display/console_node.h"

#include "core/log.h"
#include "render/backend/render_backend.h"
#include "render/backend/render_extension.h"
#include "render/core/shader_program.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <format>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {
  const Logger kLog("console");

  constexpr char kVertexShader[] = R"(
precision highp float;
attribute vec2 a_position;
uniform vec2 u_surface_size;
uniform vec2 u_quad_size;
uniform mat3 u_transform;
uniform vec4 u_uv;
varying vec2 v_uv;
void main() {
    vec3 pixel = u_transform * vec3(a_position * u_quad_size, 1.0);
    vec2 normalized = pixel.xy / u_surface_size;
    v_uv = mix(u_uv.xy, u_uv.zw, a_position);
    gl_Position = vec4(normalized.x * 2.0 - 1.0, 1.0 - normalized.y * 2.0, 0.0, 1.0);
}
)";

  // A guest's picture has no transparency; the X of XRGB is whatever the guest left there. Its
  // cursor has, premultiplied.
  constexpr char kFragmentShader[] = R"(
precision mediump float;
uniform sampler2D u_texture;
uniform float u_opacity;
uniform float u_alpha;
varying vec2 v_uv;
void main() {
    vec4 color = texture2D(u_texture, v_uv);
    gl_FragColor = mix(vec4(color.rgb, 1.0), color, u_alpha) * u_opacity;
}
)";

  // Nodes die without a GL context at hand; their textures are freed with the next draw.
  std::mutex g_releasedMutex;
  std::vector<std::uint64_t> g_released;
  std::atomic<std::uint64_t> g_nextId{1};

  struct Slot {
    GLuint texture = 0;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    std::uint64_t serial = 0;
    std::uint32_t width = 0, height = 0;
    bool usable = false;
    // GL_LINEAR or GL_NEAREST once set; 0 for a texture whose filter was never set.
    GLint filter = 0;
  };

  // The shader and the textures of every console; owned by the render backend, so they follow its
  // teardown and the loss of the graphics context.
  class ConsoleProgram final : public RenderExtension {
  public:
    void ensureInitialized() {
      if (m_program.isValid()) {
        return;
      }
      m_program.create(kVertexShader, kFragmentShader);
      m_position = glGetAttribLocation(m_program.id(), "a_position");
      m_surfaceSize = glGetUniformLocation(m_program.id(), "u_surface_size");
      m_quadSize = glGetUniformLocation(m_program.id(), "u_quad_size");
      m_transform = glGetUniformLocation(m_program.id(), "u_transform");
      m_uv = glGetUniformLocation(m_program.id(), "u_uv");
      m_opacity = glGetUniformLocation(m_program.id(), "u_opacity");
      m_alpha = glGetUniformLocation(m_program.id(), "u_alpha");
      m_createImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
      m_destroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
      m_imageTarget =
          reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    }

    void destroy() override {
      for (auto& [id, slot] : m_slots) {
        free(slot);
      }
      for (auto& [id, slot] : m_cursors) {
        free(slot);
      }
      m_slots.clear();
      m_cursors.clear();
      m_program.destroy();
    }

    void abandon() noexcept override {
      m_slots.clear();
      m_cursors.clear();
      m_program.abandon();
    }

    Slot& slot(std::uint64_t id) {
      std::vector<std::uint64_t> released;
      {
        std::lock_guard lock(g_releasedMutex);
        released.swap(g_released);
      }
      for (const auto gone : released) {
        if (const auto it = m_slots.find(gone); it != m_slots.end()) {
          free(it->second);
          m_slots.erase(it);
        }
        if (const auto it = m_cursors.find(gone); it != m_cursors.end()) {
          free(it->second);
          m_cursors.erase(it);
        }
      }
      return m_slots[id];
    }

    // The guest's GPU buffer as a texture, with no copy. An empty string when it worked.
    std::string import(Slot& slot, const coffret::Scanout& scanout) {
      free(slot);
      if (!m_createImage || !m_destroyImage || !m_imageTarget) {
        return "this graphics driver cannot import GPU buffers";
      }
      static constexpr std::array<std::array<EGLint, 5>, 4> kPlaneAttributes{{
          {EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE0_PITCH_EXT,
           EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT},
          {EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
           EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT},
          {EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT,
           EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT},
          {EGL_DMA_BUF_PLANE3_FD_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT,
           EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT},
      }};
      // 0x00ffffffffffffff is DRM_FORMAT_MOD_INVALID: the buffer has no explicit modifier.
      const bool modifier = scanout.modifier != 0x00ffffffffffffffULL;
      std::vector<EGLint> attributes{
          EGL_WIDTH, static_cast<EGLint>(scanout.backingWidth), EGL_HEIGHT, static_cast<EGLint>(scanout.backingHeight),
          EGL_LINUX_DRM_FOURCC_EXT, static_cast<EGLint>(scanout.fourcc),
      };
      for (std::size_t i = 0; i < scanout.planes.size() && i < kPlaneAttributes.size(); ++i) {
        const auto& plane = scanout.planes[i];
        const auto& names = kPlaneAttributes[i];
        attributes.insert(attributes.end(), {names[0], plane.fd, names[1], static_cast<EGLint>(plane.offset), names[2],
                                             static_cast<EGLint>(plane.stride)});
        if (modifier) {
          attributes.insert(attributes.end(), {names[3], static_cast<EGLint>(scanout.modifier & 0xffffffffU), names[4],
                                               static_cast<EGLint>(scanout.modifier >> 32)});
        }
      }
      attributes.push_back(EGL_NONE);
      slot.image = m_createImage(eglGetCurrentDisplay(), EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attributes.data());
      if (slot.image == EGL_NO_IMAGE_KHR) {
        return std::format(
            "the guest's GPU buffer could not be imported (EGL error {:#x}, format {:#x}, modifier {:#x}, {} planes)",
            eglGetError(), scanout.fourcc, scanout.modifier, scanout.planes.size()
        );
      }
      while (glGetError() != GL_NO_ERROR) {
      }
      glGenTextures(1, &slot.texture);
      glBindTexture(GL_TEXTURE_2D, slot.texture);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      m_imageTarget(GL_TEXTURE_2D, slot.image);
      if (const GLenum error = glGetError(); error != GL_NO_ERROR) {
        free(slot);
        return std::format("the guest's GPU buffer could not be used as a texture (GL error {:#x})", error);
      }
      slot.usable = true;
      applyFilter(slot, true);
      return {};
    }

    // The shared pixels as a texture: all of them for a new picture, else the rows that changed.
    void upload(Slot& slot, const coffret::Scanout& scanout, const coffret::Rect& damage, bool fresh) {
      GLint alignment = 4;
      glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
      glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
      glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, static_cast<GLint>(scanout.stride / 4));
      if (fresh || !slot.usable || slot.width != scanout.width || slot.height != scanout.height) {
        free(slot);
        glGenTextures(1, &slot.texture);
        glBindTexture(GL_TEXTURE_2D, slot.texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(
            GL_TEXTURE_2D, 0, GL_BGRA_EXT, static_cast<GLsizei>(scanout.width), static_cast<GLsizei>(scanout.height), 0,
            GL_BGRA_EXT, GL_UNSIGNED_BYTE, scanout.pixels
        );
        slot.width = scanout.width;
        slot.height = scanout.height;
        slot.usable = true;
        applyFilter(slot, true);
      } else if (!damage.empty()) {
        glBindTexture(GL_TEXTURE_2D, slot.texture);
        const auto* first = scanout.pixels + static_cast<std::size_t>(damage.y) * scanout.stride
                          + static_cast<std::size_t>(damage.x) * 4;
        glTexSubImage2D(GL_TEXTURE_2D, 0, damage.x, damage.y, damage.width, damage.height, GL_BGRA_EXT, GL_UNSIGNED_BYTE, first);
      }
      glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, 0);
      glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
    }

    // The texture's content into a PPM file, top row first.
    void dump(Slot& slot, std::uint32_t width, std::uint32_t height, bool bottomUp, const std::string& path) {
      if (width == 0 || height == 0) {
        return;
      }
      GLint previous = 0;
      glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
      GLuint framebuffer = 0;
      glGenFramebuffers(1, &framebuffer);
      glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, slot.texture, 0);
      if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        if (std::FILE* file = std::fopen(path.c_str(), "wb")) {
          std::fprintf(file, "P6\n%u %u\n255\n", width, height);
          std::vector<std::uint8_t> row(static_cast<std::size_t>(width) * 3);
          for (std::uint32_t y = 0; y < height; ++y) {
            const std::uint8_t* source = pixels.data() + static_cast<std::size_t>(bottomUp ? height - 1 - y : y) * width * 4;
            for (std::uint32_t x = 0; x < width; ++x) {
              row[x * 3] = source[x * 4];
              row[x * 3 + 1] = source[x * 4 + 1];
              row[x * 3 + 2] = source[x * 4 + 2];
            }
            std::fwrite(row.data(), 1, row.size(), file);
          }
          std::fclose(file);
        }
      }
      glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previous));
      glDeleteFramebuffers(1, &framebuffer);
    }

    // The guest's cursor over its picture, where the guest has it.
    void drawCursor(std::uint64_t id, const coffret::GuestCursor& cursor, const CustomDrawContext& context, const coffret::Placement& picture, float guestWidth, float guestHeight) {
      if (!cursor.visible || !cursor.pixels || cursor.width <= 0 || cursor.height <= 0 || guestWidth <= 0 || guestHeight <= 0) {
        return;
      }
      Slot& slot = m_cursors[id];
      if (slot.texture == 0 || slot.serial != cursor.serial) {
        if (slot.texture == 0) {
          glGenTextures(1, &slot.texture);
        }
        glBindTexture(GL_TEXTURE_2D, slot.texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_BGRA_EXT, cursor.width, cursor.height, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, cursor.pixels->data());
        slot.serial = cursor.serial;
        slot.filter = 0;
        slot.usable = true;
      }
      const float scaleX = picture.width / guestWidth, scaleY = picture.height / guestHeight;
      coffret::Placement placement;
      placement.x = picture.x + static_cast<float>(cursor.x) * scaleX;
      placement.y = picture.y + static_cast<float>(cursor.y) * scaleY;
      placement.width = static_cast<float>(cursor.width) * scaleX;
      placement.height = static_cast<float>(cursor.height) * scaleY;
      placement.exact = picture.exact;
      const GLboolean blending = glIsEnabled(GL_BLEND);
      GLint sourceRgb = 0, destinationRgb = 0, sourceAlpha = 0, destinationAlpha = 0;
      glGetIntegerv(GL_BLEND_SRC_RGB, &sourceRgb);
      glGetIntegerv(GL_BLEND_DST_RGB, &destinationRgb);
      glGetIntegerv(GL_BLEND_SRC_ALPHA, &sourceAlpha);
      glGetIntegerv(GL_BLEND_DST_ALPHA, &destinationAlpha);
      glEnable(GL_BLEND);
      glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
      drawSlot(slot, context, placement, {0.0F, 0.0F, 1.0F, 1.0F}, 1.0F);
      glBlendFuncSeparate(
          static_cast<GLenum>(sourceRgb), static_cast<GLenum>(destinationRgb), static_cast<GLenum>(sourceAlpha),
          static_cast<GLenum>(destinationAlpha)
      );
      if (!blending) {
        glDisable(GL_BLEND);
      }
    }

    void drawSlot(
        Slot& slot, const CustomDrawContext& context, const coffret::Placement& placement, const std::array<float, 4>& uv,
        float alpha = 0.0F
    ) {
      if (!slot.usable || !m_program.isValid()) {
        return;
      }
      glBindTexture(GL_TEXTURE_2D, slot.texture);
      applyFilter(slot, !placement.exact);
      static constexpr std::array<GLfloat, 12> kQuad{0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 1.0F, 1.0F, 0.0F, 1.0F, 1.0F};
      const Mat3 transform = context.transform * Mat3::translation(placement.x, placement.y);
      glUseProgram(m_program.id());
      glActiveTexture(GL_TEXTURE0);
      glUniform2f(m_surfaceSize, context.surfaceWidth, context.surfaceHeight);
      glUniform2f(m_quadSize, placement.width, placement.height);
      glUniformMatrix3fv(m_transform, 1, GL_FALSE, transform.m.data());
      glUniform4f(m_uv, uv[0], uv[1], uv[2], uv[3]);
      glUniform1f(m_opacity, context.opacity);
      glUniform1f(m_alpha, alpha);
      const auto position = static_cast<GLuint>(m_position);
      glVertexAttribPointer(position, 2, GL_FLOAT, GL_FALSE, 0, kQuad.data());
      glEnableVertexAttribArray(position);
      glDrawArrays(GL_TRIANGLES, 0, 6);
      glDisableVertexAttribArray(position);
    }

  private:
    // For the texture that is bound.
    void applyFilter(Slot& slot, bool linear) {
      const GLint filter = linear ? GL_LINEAR : GL_NEAREST;
      if (slot.filter == filter) {
        return;
      }
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
      slot.filter = filter;
    }

    void free(Slot& slot) {
      if (slot.texture != 0) {
        glDeleteTextures(1, &slot.texture);
      }
      if (slot.image != EGL_NO_IMAGE_KHR && m_destroyImage) {
        m_destroyImage(eglGetCurrentDisplay(), slot.image);
      }
      slot = Slot{};
    }

    ShaderProgram m_program;
    GLint m_position = -1, m_surfaceSize = -1, m_quadSize = -1, m_transform = -1, m_uv = -1, m_opacity = -1, m_alpha = -1;
    std::unordered_map<std::uint64_t, Slot> m_cursors;
    PFNEGLCREATEIMAGEKHRPROC m_createImage = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC m_destroyImage = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC m_imageTarget = nullptr;
    std::unordered_map<std::uint64_t, Slot> m_slots;
  };
} // namespace

coffret::ConsoleNode::ConsoleNode(std::shared_ptr<FrameStore> frames) : m_frames(std::move(frames)), m_id(g_nextId++) {}

coffret::ConsoleNode::~ConsoleNode() {
  std::lock_guard lock(g_releasedMutex);
  g_released.push_back(m_id);
}

coffret::Placement coffret::ConsoleNode::placement(float scale) const {
  const Scanout scanout = m_frames->peek();
  return placeFrame(width(), height(), scale, scanout.width, scanout.height);
}

void coffret::ConsoleNode::draw(RenderBackend& backend, const CustomDrawContext& context) const {
  auto& program = backend.extension<ConsoleProgram>();
  program.ensureInitialized();
  Slot& slot = program.slot(m_id);
  const FrameStore::Snapshot snapshot = m_frames->take();
  const Scanout& scanout = snapshot.scanout;
  const bool fresh = scanout.serial != slot.serial;
  std::array<float, 4> uv{0.0F, 0.0F, 1.0F, 1.0F};

  if (scanout.kind == Scanout::Kind::None) {
    if (fresh) {
      slot.usable = false;
      slot.serial = scanout.serial;
      m_stats.path = "none";
      m_stats.width = m_stats.height = 0;
    }
    return;
  }
  if (scanout.kind == Scanout::Kind::Dmabuf) {
    if (fresh) {
      const std::string problem = program.import(slot, scanout);
      // After the import, which starts from an empty slot.
      slot.serial = scanout.serial;
      if (!problem.empty()) {
        kLog.warn("{}", problem);
      }
      m_stats.path = problem.empty() ? "dmabuf" : "dmabuf failed: " + problem;
      kLog.debug(
          "dmabuf {}x{} of {}x{} at {},{} fourcc {:#x} modifier {:#x} planes {} y0_top {}", scanout.width, scanout.height,
          scanout.backingWidth, scanout.backingHeight, scanout.x, scanout.y, scanout.fourcc, scanout.modifier,
          scanout.planes.size(), scanout.y0Top
      );
    }
    const float bw = static_cast<float>(std::max(1U, scanout.backingWidth)), bh = static_cast<float>(std::max(1U, scanout.backingHeight));
    uv = {static_cast<float>(scanout.x) / bw, static_cast<float>(scanout.y) / bh,
          static_cast<float>(scanout.x + scanout.width) / bw, static_cast<float>(scanout.y + scanout.height) / bh};
    // QEMU's y0_top is about a GL consumer, whose origin is at the bottom. Drawn from the top down
    // as here it is the other way round: with virgl y0_top is false and the first row is the top.
    if (scanout.y0Top) {
      uv[1] = 1.0F - uv[1];
      uv[3] = 1.0F - uv[3];
    }
  } else {
    if (scanout.pixmanFormat != kPixmanX8R8G8B8 && scanout.pixmanFormat != kPixmanA8R8G8B8) {
      if (fresh) {
        slot.serial = scanout.serial;
        slot.usable = false;
        m_stats.path = std::format("pixel format {:#x} is not supported", scanout.pixmanFormat);
        kLog.warn("{}", m_stats.path);
      }
      return;
    }
    program.upload(slot, scanout, snapshot.damage, fresh);
    slot.serial = scanout.serial;
    m_stats.path = "shared memory";
  }
  if (fresh || !snapshot.damage.empty()) {
    ++m_stats.frames;
  }
  if (!m_dumpPath.empty() && slot.usable) {
    program.dump(slot, scanout.width, scanout.height, scanout.kind == Scanout::Kind::Dmabuf && scanout.y0Top, std::exchange(m_dumpPath, {}));
  }
  m_stats.width = scanout.width;
  m_stats.height = scanout.height;
  const float scale = std::max(context.pixelScaleX, 0.01F);
  const Placement picture = placeFrame(context.width, context.height, scale, scanout.width, scanout.height);
  program.drawSlot(slot, context, picture, uv);
  if (m_drawCursor) {
    program.drawCursor(m_id, m_frames->cursor(), context, picture, static_cast<float>(scanout.width), static_cast<float>(scanout.height));
  }
}
