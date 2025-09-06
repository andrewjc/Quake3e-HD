// Portal System Shaders
// Visual effects for orange and blue portals

// Orange Portal Ring
portal/orange_ring
{
    {
        map textures/portal/energy_swirl.tga
        blendFunc GL_ONE GL_ONE
        rgbGen wave sin 0.8 0.2 0 0.5
        tcMod rotate 30
        tcMod scale 2 2
    }
    {
        map textures/portal/ring_overlay.tga
        blendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
        rgbGen const ( 1.0 0.5 0.0 )
        alphaGen wave sin 0.7 0.3 0 1
    }
}

// Blue Portal Ring
portal/blue_ring
{
    {
        map textures/portal/energy_swirl.tga
        blendFunc GL_ONE GL_ONE
        rgbGen wave sin 0.8 0.2 0 0.5
        tcMod rotate -30
        tcMod scale 2 2
    }
    {
        map textures/portal/ring_overlay.tga
        blendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
        rgbGen const ( 0.0 0.5 1.0 )
        alphaGen wave sin 0.7 0.3 0 1
    }
}

// Portal Surface Effect (view through portal)
portal/surface
{
    sort portal
    surfaceparm nolightmap
    portal
    {
        map $whiteimage
        blendFunc GL_ONE GL_ONE_MINUS_SRC_COLOR
        depthWrite
    }
}

// Portal Ring Generic
portal/ring
{
    cull disable
    surfaceparm trans
    surfaceparm nomarks
    surfaceparm nolightmap
    deformVertexes wave 100 sin 0 1 0 0.5
    {
        map textures/portal/ring_gradient.tga
        blendFunc GL_SRC_ALPHA GL_ONE
        rgbGen vertex
        alphaGen vertex
        tcMod stretch sin 0.95 0.1 0 0.5
    }
}

// Portal Glow Effect
portal/glow
{
    deformVertexes autoSprite
    surfaceparm trans
    surfaceparm nomarks
    surfaceparm nolightmap
    cull disable
    {
        map textures/portal/glow.tga
        blendFunc GL_SRC_ALPHA GL_ONE
        rgbGen vertex
        alphaGen vertex
    }
}

// Portal Opening Effect
portal/opening
{
    surfaceparm trans
    surfaceparm nomarks
    surfaceparm nolightmap
    cull disable
    {
        animMap 10 textures/portal/open1.tga textures/portal/open2.tga textures/portal/open3.tga textures/portal/open4.tga
        blendFunc GL_SRC_ALPHA GL_ONE
        rgbGen vertex
        alphaGen wave inversesawtooth 0 1 0 10
    }
}

// Portal Particle
portal/particle
{
    cull disable
    surfaceparm trans
    surfaceparm nomarks
    surfaceparm nolightmap
    {
        map textures/portal/spark.tga
        blendFunc GL_SRC_ALPHA GL_ONE
        rgbGen vertex
        alphaGen vertex
    }
}

// Portal Edge Distortion
portal/edge_distortion
{
    deformVertexes wave 100 sin 0 2 0 0.25
    surfaceparm trans
    surfaceparm nomarks
    surfaceparm nolightmap
    {
        map textures/effects/envmap.tga
        tcGen environment
        blendFunc GL_ONE GL_ONE
        rgbGen wave sin 0.3 0.1 0 0.5
        tcMod scale 3 3
        tcMod scroll 0.1 0.1
    }
}

// Orange Portal Projectile
portal/projectile_orange
{
    cull disable
    surfaceparm trans
    surfaceparm nomarks
    surfaceparm nolightmap
    {
        map textures/portal/plasma_orange.tga
        blendFunc GL_ONE GL_ONE
        rgbGen const ( 1.0 0.5 0.0 )
        tcMod rotate 360
    }
}

// Blue Portal Projectile
portal/projectile_blue
{
    cull disable
    surfaceparm trans
    surfaceparm nomarks
    surfaceparm nolightmap
    {
        map textures/portal/plasma_blue.tga
        blendFunc GL_ONE GL_ONE
        rgbGen const ( 0.0 0.5 1.0 )
        tcMod rotate -360
    }
}

// Portal Impact Effect
portal/impact
{
    surfaceparm trans
    surfaceparm nomarks
    surfaceparm nolightmap
    cull disable
    {
        animMap 20 textures/portal/impact1.tga textures/portal/impact2.tga textures/portal/impact3.tga
        blendFunc GL_SRC_ALPHA GL_ONE
        rgbGen vertex
        alphaGen wave inversesawtooth 0 1 0 20
    }
}

// Portal Surface No-Portal Material (prevents portal placement)
textures/common/noportal
{
    surfaceparm noportal
    surfaceparm nomarks
    {
        map $lightmap
        rgbGen identity
    }
    {
        map textures/common/noportal.tga
        blendFunc GL_DST_COLOR GL_ZERO
        rgbGen identity
    }
}