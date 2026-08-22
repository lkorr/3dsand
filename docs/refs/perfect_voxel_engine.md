# The Perfect Voxel Engine

**John Lin**, voxely.net — published September 18, 2021.
Source: <https://voxely.net/blog/the-perfect-voxel-engine/>

Mirrored here because CLAUDE.md's "Architecting a new system" section is
distilled from it and the original is a personal blog that has been dormant
since 2023. Article text below; comments omitted.

---

## More Is Better, Right?

What is it about voxels that makes people go crazy? Throughout the past decade,
there have been SO many developers obsessed with shrinking them down to have as
many as possible, admittedly including myself. It's exciting both as a developer
and to the gaming community to see amazing algorithms produce so much detail and
think about the possibilities it brings to virtual worlds.

Voxels = destruction!
Voxels = building!
Voxels = the universe!

And yet, there's no commercially available, general-purpose, widely-used voxel
engines which games are built on. The term "(micro) voxel engine" is basically
synonymous with vaporware. We see jaw-dropping showcases that are sometimes
accompanied by hyperbolic claims ("UNLIMITED DETAIL") and then radio silence.
But why?

## Defining the Problem

A lot of voxel developers base their algorithms on their rendering capabilities.
If you can rasterize (dual contouring), ray cast (Voxlap), splat/frustum trace
(Euclideon), sphere march (SDFs/isosurfaces), ray trace (sparse voxel octrees),
or somehow get billions of voxels from memory to pixels on your screen, then the
most important part is done, right?

Let's briefly consider what additional capabilities a voxel engine should
provide in order to create a game:

- Lighting
- State serialization and synchronization across a network
- Physics/collision detection
- AI features
- Dynamic objects

These are all systems that operate on the existing data. This isn't even
including the building and modification of the game world. To create anything
resembling reality, the world usually also needs:

- Terrain with interesting features
- Trees
- Vegetation
- Water
- Artificial structures

Perhaps most importantly, people tend to expect or look forward to voxel engines
with the following features:

- Creating anything
- Destroying everything
- Procedural generation
- Physical interactions on a per-voxel level
- Alternate simulations (anything else that interacts with voxel data, e.g.
  vegetation growth, door responses, "logic" like Minecraft's redstone and
  pistons, and so on)
- Voxel characters

And these are really just core features. While not strictly engine-related,
there's still gameplay to integrate that provides the user with a fulfilling way
to experience what the engine can do. There's also art direction, sound design,
atmosphere and tone, and other architecture details like system compatibility,
performance, codebase management, scalability, the developer experience, mod
support, and more. It's important to think about these things because the engine
needs to be sufficiently designed to achieve these goals.

## It's All About the Data

While writing an awesome voxel renderer is certainly no easy feat, we can see
that it's only a small part in the grand scheme of systems that need to work
together. We need to choose a voxel format that can be rendered efficiently, but
how can we make sure it works for everything else as well?

Sparse voxel octrees might be able to hold a couple billion voxels worth of data
that we can cast primary and shadow rays against, but how well do they work for
collision detection? Global illumination? Path finding? Adding new per-voxel
attributes besides just albedo and normals? Dynamic objects?

It's important to answer these questions ahead of time because most of the
systems we build need to incorporate the format into their designs. As it turns
out for sparse voxel octrees, storage and rendering are the only things they are
acceptable (not even great) at. Moreover, if systems are built on top of the
general volume design and that design changes, then a lot of time is going to
end up spent updating the entire codebase to work with it.

## Modular By Design

In the previous post I talked about how an engine can be designed around ECS
principles to enable flexibility and embrace expandability in its systems. This
post is going to continue that philosophy to solve the voxel format problem.

The solution is actually rather obvious: to use whatever voxel format is best
for the job! This means not having one or two, but as many as are necessary.

Actually, the credit for this idea comes from graphics programming – we see it
successfully used with 3d models both with file protocols (obj, ply, fbx, etc.)
and with shaders! Game engines are able to use a library like assimp to import
basically any model into a common format, and then convert that format into
whatever the GPU needs to rasterize the triangles.

Going beyond just graphics programming, many physics and ray tracing libraries
are built around triangle data (vertices and indices). That is the core common
"raw" format which makes up the physical geometry. Supplementary attributes can
be stored alongside this data, like texture coordinates for use in the fragment
shader or material identifiers for looking up friction coefficients on the
physics side.

## A Thought Experiment

Imagine you're writing a ray tracing API that accepts a list of vertices making
up a triangle mesh, which then builds an acceleration structure and bakes the
vertex attributes into the leaf nodes. Your input looks like this:

```cpp
struct vertex_t
{
    vec3 position;
    vec3 normal;
};
acceleration_structure_t* build_structure(vertex_t* vertices, uint32_t num_vertices) { ... }
```

Suppose one day you wanted to include colors with the vertices as well. Would
the correct solution be:

A) Add `vec3 color;` to the vertex structure, modify the `build_structure` code
to account for this new data, and adjust all relevant code to look for the
colors at the end of each vertex.

B) Adjust `build_structure` to accept any data type and have the user specify
where in this data the position attribute is via offset and stride, and then
switch to baking vertex indexes into the acceleration structure instead so code
that cares to use the colors can be adjusted.

If you guessed B, you'd be correct! The answer was rather obvious in this silly
experiment, but imagine replacing Vertex with Voxel and suddenly with answer A
you've described Efficient Sparse Voxel Octrees!

Now of course, baking in attributes is an implementation detail and follow-up
works that attach attributes to DAGs use an index-based approach, but that's
hardly the problem here.

Voxels in the traditional sense use explicit data sampled in an implicit space,
meaning the geometry is based on how you assign values in a grid. This means
that in many cases, we won't have a `vec3 position;` with which to build our ray
tracing API, but instead, we'll have a grid of attributes! Here's a more
realistic example.

```cpp
struct voxel_grid_t
{
    uint8_t material_ids[16 * 16 * 16];
};
```

Well that's all fine and dandy, but at some point we're going to need more than
material identifiers. With small single-colored voxels, we actually need unique
per-voxel attributes in addition to a material identifier, like albedo and
normal. For gameplay, maybe we want to have procedural vegetation, and
"redstone" logic simulated on the voxel scale. Are we going to store that data
everywhere?

```cpp
struct dumb_voxel_grid_t
{
    const size_t size = 16 * 16 * 16;
    uint8_t material_ids[size];
    vec3 albedo[size];
    vec3 normal[size];
    uint8_t vegetation_type[size];
    uint8_t vegetation_state[size];
    uint8_t redstone_strength[size];
};
```

That would be, as the struct name suggests, dumb. Memory issues aside, how is
this supposed to scale up? Sure, we could just have generic data tacked on
that's identified by the material, but that's wasteful and limited. What if
modders want to add voxel data of their own? What if we want to bitcrush the
attributes? What if some of the geometry we render is procedural, like fire?
What data belongs on the GPU and what doesn't? What gets serialized? More
importantly, what even defines the geometry here?

## There's A Point, I Promise

We've done a lot of talking about what the problems are and not a whole lot of
proactive thinking to address them. However, I believe understanding the problem
is more important than coming up with the solution itself.

I'm going to digress for a moment. Back when Steve Jobs revealed the iPhone back
in 2007, he made some remarks about adding physical buttons to the smartphones
of the day:

> And what happens when you think of a great idea six months from now? You can't
> run around an add a button to these things – they're already shipped! So what
> do you do? It doesn't work because the buttons and the controls can't change.
> They can't change for each application, and they can't change down the road if
> you think of another great idea you want to add to this product.
>
> — Steve Jobs (2007)

This sounds an awful lot like the problems that we have with our voxel designs
but with buttons instead of data. And no, I'm not just trying to make some
clichéd remarks. We need a solution that anticipates the developers' needs and
is going to allow us to dynamically build specially optimized systems.

## Back To the Format Discussion

Like the OO-ECS concept, my main goal was to design everything in such a way
that no system was closed-ended, and that developers could always iterate on or
expand the engine without any codebase overhauls.

We've observed the main problems with voxel engines being how our data is laid
out and processed, and what happens when it's not optimal for a given job. We've
also discussed how this problem is handled with traditional graphics and meshes,
and that's by having a common, bare-minimum format with which to convert to
other desired formats.

Now it's time to apply these ideas to voxels. As I previously mentioned, the
answer is simply to use whatever format is best for the job. But when working
with volumes, it's not that easy. Even with different formats, we can't ever
just store raw data for the entire world because of memory usage. Conversely, we
can't always work with compressed data because compression takes place after the
raw data has been filled out.

Lastly there's the need to dynamically add attributes. We don't want to store
vegetation growth state deep underground in some cave where vegetation isn't
growing anyway. As part of "future-proofing" the engine, developers need to be
able to add any type of attribute they want without worrying about how it fits
with the rest of the engine.

In summary, what we need is a way of working with raw/common data when we need
it, adding arbitrary per-voxel data only where we want it, and compressing the
data when we're done.

## Three Tools – Allocation, Tagging, Conversion

We can achieve all of the above through a general volume pipeline, described as
Allocation, Tagging, and Conversion. I'll talk about each one below, and give an
example of how they're used to convert some offline terrain data blocks into a
common format.

### Allocation

Some voxel data needs to exist on the GPU. Some needs to be written to the hard
drive. Some may only need to exist for a short time while we work on it. The
allocation stage is responsible for creating a buffer that voxel data can be
written to and dealing with it afterwards. It isn't the developer's
responsibility to manage any of the lower-level tasks associated with this
allocation besides returning it to the allocator when finished.

```cpp
auto allocator = ((volume_allocator_context_t*)context_t::m_engine->get_context("volume_allocator_context"))->get_volume_allocator_from_name("cpu_recycled");
volume_data_t* dest_volume = allocator->new_volume();
```

The `cpu_recycled` allocator provides the buffer for a short time while we work
on it. If we wanted the data to be sent to the GPU, we would specify that
allocator instead. But the allocator ends up being generalized and can be
swapped out based on our needs.

### Tagging

We next assign attributes and tie voxel data to the allocations. Attributes
consist of a name, size, data type, and optional data pointer.

```cpp
// Albedo
dest->header.attributes[0].bits_per_element = sizeof(albedo_t) * 8u;
dest->header.attributes[0].type = volume_attribute_types::to_enum<albedo_t>::value;
dest->header.attributes[0].total_size_in_bytes = aabb_volume * sizeof(albedo_t);
dest->header.attributes[0].set_name("albedo");

// Normal
dest->header.attributes[1].bits_per_element = sizeof(normal_t) * 8u;
dest->header.attributes[1].type = volume_attribute_types::to_enum<normal_t>::value;
dest->header.attributes[1].total_size_in_bytes = aabb_volume * sizeof(normal_t);
dest->header.attributes[1].set_name("normal");
```

There's some template magic going on here for `albedo_t` and `normal_t` in order
to allow us to change data types on the fly.

As mentioned, attributes contain a data type (which mirrors GLSL's primitive
types, or is labeled "custom"), but the engine framework preloads some common
types. For albedo and normal, this looks like:

```cpp
// Albedo
{
    attr.set_name("albedo");
    attr.bits_per_element = sizeof(u8vec4) * 8u;
    attr.type = VOLUME_ATTRIBUTE_TYPE_U8VEC4;
    add_attribute(&attr, "albedo");
}

// Normal
{
    attr.set_name("normal");
    attr.bits_per_element = sizeof(vec3) * 8u;
    attr.type = VOLUME_ATTRIBUTE_TYPE_VEC3;
    add_attribute(&attr, "normal");
}
```

The pseudo-reflection part happens here:

```cpp
if (m_albedo_template->type == VOLUME_ATTRIBUTE_TYPE_U8VEC4 && m_normal_template->type == VOLUME_ATTRIBUTE_TYPE_VEC3)
{
    m_standard_conversion = std::bind(&terrain_block_conversion::convert_terrain_block_to_static_standard<u8vec4, vec3>, this, _1, _2, _3);
}
```

Admittedly, this part is a little weak because it requires pre-coding the
supported type combos. We are able to change it on the fly by changing the
function pointer, but we still have to declare the template function so the
compiler can actually generate the function. While I'm hoping to improve this
part down the line, it works great by having compile-time conversions in place.

### Conversion

At the heart of the solution is arguably the most important stage which is
responsible for converting voxel data from one format to the next.

```cpp
allocator->allocate_volume(dest);

binary_writer_t albedo_writer = binary_writer_t(dest->get_data_from_index(0), dest->header.attributes[0].total_size_in_bytes);
binary_writer_t normal_writer = binary_writer_t(dest->get_data_from_index(1), dest->header.attributes[1].total_size_in_bytes);

for (int x = 0; x < aabb_size.x; x++)
{
    for (int y = 0; y < aabb_size.y; y++)
    {
        for (int z = 0; z < aabb_size.z; z++)
        {
            u8vec4 src_albedo = cell_reader.read_u8vec4();
            albedo_t dest_albedo = attribute_converter<u8vec4, albedo_t>::convert(src_albedo);
            albedo_writer.write(dest_albedo);

            vec3 src_normal = cell_reader.read_vec3();
            normal_t dest_normal = attribute_converter<vec3, normal_t>::convert(src_normal);
            normal_writer.write(dest_normal);
        }
    }
}
```

This starts off by calling the volume's allocation function, which performs any
behind-the-scenes allocations to prepare the data specified by the attributes.

This terrain block conversion expects there to be a 3D array of albedo and
normal data made up of a `u8vec4` and `vec3`, respectively. It then writes to
the attributes using whatever type was specified for them. In this instance, the
conversion operator is reading the `terrain_block` format, and converting to the
default format, which is simply raw attributes laid out for the entire volume.

Attribute type conversion is handled here as well, but again optimized thanks to
templates. By default, values are simply casted, but more often than not we want
a more sophisticated conversion. For example, converting a `u8vec4` to a `vec4`:

```cpp
template<>
class attribute_converter<glm::u8vec4, glm::vec4>
{
public:
    static inline glm::vec4 convert(glm::u8vec4 input)
    {
        glm::vec4 v = input;
        v *= 1.0f / 255.0f;
        return v;
    }
};
```

Finally, we just make sure to call the conversion function and cleanup
afterwards. The format source and destination is specified here too:

```cpp
auto fn_conversion = ((volume_conversion_context_t*)(context_t::m_engine->get_context("volume_conversion_context")))->get_conversion("terrain_cell", "default");
...
(*fn_conversion)(src_volume, dest_volume, allocator);
allocator->return_volume(dest_volume);
```

The usage of function pointers for these things is talked about in the last
post, but I'll mention it here as well – I want these systems to be
language-agnostic. The core of the engine will be written in C++ for performance
reasons, but content developers may want to whip something up in C# (which
everything has bindings for). And so it's important that that avenue, and thus
by extension other languages that can call C functions and generate C function
pointers, remains open.

Also, while the above example focused on a rather trivial conversion, by tagging
attributes and including their formats, compressors and other conversion
operators can be made to work on general data. There are some other indicators
that hint how data can be compressed, such as special entries in the type enum
and the `bits_per_element` field. When working with explicit volumes, general
format conversion for the entire volume is great, and then separate volumes
would be used for the special data.

## What Data Conversion Has To Do With Anything

Data conversion has everything to do with anything. By building systems around
formats that can be swapped out at run time or exchanged/changed in the future,
your codebase is safe from painful refactors. Experimenting with a new format
means just writing a conversion from the old format to the new. Maybe you want
to forego any conversion overhead in a specific system – so extend that system
to be compatible with your compressed data.

The power of the system goes beyond just saving your codebase, too. Conversion
operators can also be written for:

- Importing meshes and voxelizing them
- Importing Minecraft maps to their detailed voxel counterparts
- Converting CSG operations to voxels (AKA a building system)
- Importing old versions of game maps
- Creating more effective compressors
- Generating collision data for physics processing
- Voxelizing procedurally generated terrain
- Converting compressed or raw data to an offline or network-ready format
- Generating voxel vegetation by converting "seed data" into voxels

So as you can see, conversion operators aren't just for rearranging data;
they're more like black boxes. Input data of a specific format and let the engine
figure out and execute the link to the desired output format. It's such a simple
concept that's more general than volume conversion, and yet it's managed to solve
our problems.

## One Missing Key Component

Rendering: the very thing we started out discussing and are going to close on.
How do we take a dynamic voxel format with dynamic attributes and expect to throw
it at the GPU?

The solution here will have to wait until the next post where I take a deep dive
into the rendering setup (I guess I accidentally lied on the last post). I'll
briefly cover it here though.

The following is generally true for OptiX, DXR, Embree, or Vulkan, but I'll
focus on Vulkan because it's the greatest programming API to ever exist (more on
that in the next post).

The shaders that get executed in the ray tracing pipeline depend on four things:

1. The geometry configuration in the BLAS
2. The SBT offset in the top level instance
3. The shader binding table's data
4. The geometry that was potentially hit

By tracking the voxel formats that make up a BLAS' geometries, we can build the
SBT with specific intersection shaders that have been tailored to the format
design. Callable shaders can also be bound in order to decode attributes that are
required in a pipeline. The end result is a very clean and again modular way of
handling different formats, and of course the engine takes care of building
these – the user just has to link a format with its intersection & callable
shaders.

## Closing Remarks

The "Perfect" Voxel Engine, at least to me, is an engine that lets developers to
just use voxels where they shine without having to deal with structures. There's
a lot that I glanced over, like how to build and deal with a large world that's
volumetric on a higher level, but I actually think it's out of the scope for this
post. The perfect voxel engine should provide the tools with which to build
bigger infrastructures on top of, which is where the game engine starts coming
into play.

In the next post, we'll (hopefully for real) dive into the rendering
architecture. It'll be all about Vulkan, which I can't wait to talk about!

*(The rendering follow-up was never published; the blog's next and last post was
"Using a RNN for 2D Tile Map Synthesis", August 2023.)*
