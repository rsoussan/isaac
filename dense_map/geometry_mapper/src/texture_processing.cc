/* Copyright (c) 2021, United States Government, as represented by the
 * Administrator of the National Aeronautics and Space Administration.
 *
 * All rights reserved.
 *
 * The "ISAAC - Integrated System for Autonomous and Adaptive Caretaking
 * platform" software is licensed under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with the
 * License. You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

#include <texture_processing.h>

// OpenCV includes
#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc.hpp>

// Eigen includes
#include <Eigen/Geometry>
#include <Eigen/Core>

// Astrobee includes
#include <ff_util/ff_names.h>
#include <ff_common/utils.h>
#include <camera/camera_model.h>
#include <config_reader/config_reader.h>

// Glog
#include <glog/logging.h>

// Boost includes
#include <boost/filesystem.hpp>

// texrecon includes
#include <mve/image_io.h>
#include <mve/image_tools.h>
#include <tex/defines.h>
#include <tex/obj_model.h>
#include <util/file_system.h>

// System includes
#include <string>
#include <map>
#include <iostream>
#include <fstream>
#include <utility>
#include <set>

namespace dense_map {

const int NUM_CHANNELS = 4;  // BRGA
const int TILE_PADDING = 4;

IsaacTextureAtlas::IsaacTextureAtlas(unsigned int width, unsigned int height)
    : width(width), height(height), determined_height(0), padding(TILE_PADDING), finalized(false) {
  bin = RectangularBin::create(width, height);
  image = mve::ByteImage::create(width, height, NUM_CHANNELS);
}

/**
 * Copies the src image into the dest image at the given position,
 * optionally adding a border.
 * @warning asserts that the given src image fits into the given dest image.
 */
void copy_into(mve::ByteImage::ConstPtr src, int x, int y, mve::ByteImage::Ptr dest, int border = 0) {
  assert(x >= 0 && x + src->width() + 2 * border <= dest->width());
  assert(y >= 0 && y + src->height() + 2 * border <= dest->height());
  assert(src->channels() == dest->channels());

  for (int i = 0; i < src->width() + 2 * border; ++i) {
    for (int j = 0; j < src->height() + 2 * border; j++) {
      int sx = i - border;
      int sy = j - border;

      if (sx < 0 || sx >= src->width() || sy < 0 || sy >= src->height()) continue;

      for (int c = 0; c < src->channels(); ++c) {
        dest->at(x + i, y + j, c) = src->at(sx, sy, c);
      }
    }
  }
}

typedef std::vector<std::pair<int, int>> PixelVector;
typedef std::set<std::pair<int, int>> PixelSet;

bool IsaacTextureAtlas::insert(IsaacTexturePatch::ConstPtr texture_patch) {
  if (finalized) throw util::Exception("No insertion possible, IsaacTextureAtlas already finalized");

  assert(bin != NULL);

  int const width = texture_patch->get_width() + 2 * padding;
  int const height = texture_patch->get_height() + 2 * padding;
  Rect<int> rect(0, 0, width, height);
  if (!bin->insert(&rect)) return false;

  // Keep track how many rows of the allocated texture we actually use
  determined_height = std::max((unsigned int)(rect.max_y), determined_height);

  // Note how we don't bother to do any copying, as at this stage we
  // only care for the structure
  // copy_into(patch_image, rect.min_x, rect.min_y, image, padding);

  IsaacTexturePatch::Faces const& patch_faces = texture_patch->get_faces();
  IsaacTexturePatch::Texcoords const& patch_texcoords = texture_patch->get_texcoords();

  /* Calculate the offset of the texture patches' relative texture coordinates */
  math::Vec2f offset = math::Vec2f(rect.min_x + padding, rect.min_y + padding);

  faces.insert(faces.end(), patch_faces.begin(), patch_faces.end());

  /* Calculate the final textcoords of the faces. */
  for (std::size_t i = 0; i < patch_faces.size(); ++i) {
    for (int j = 0; j < 3; ++j) {
      math::Vec2f rel_texcoord(patch_texcoords[i * 3 + j]);
      math::Vec2f texcoord = rel_texcoord + offset;

      // Delay this until later
      // texcoord[0] = texcoord[0] / this->width;
      // texcoord[1] = texcoord[1] / this->height;

      texcoords.push_back(texcoord);
    }
  }
  return true;
}

struct VectorCompare {
  bool operator()(math::Vec2f const& lhs, math::Vec2f const& rhs) const {
    return lhs[0] < rhs[0] || (lhs[0] == rhs[0] && lhs[1] < rhs[1]);
  }
};

typedef std::map<math::Vec2f, std::size_t, VectorCompare> TexcoordMap;

void IsaacTextureAtlas::merge_texcoords() {
  Texcoords tmp;
  tmp.swap(this->texcoords);

  // Do not remove duplicates, as that messes up the book-keeping
  TexcoordMap texcoord_map;
  for (math::Vec2f const& texcoord : tmp) {
    TexcoordMap::iterator iter = texcoord_map.find(texcoord);
    // if (iter == texcoord_map.end()) {
    std::size_t texcoord_id = this->texcoords.size();
    texcoord_map[texcoord] = texcoord_id;
    this->texcoords.push_back(texcoord);
    this->texcoord_ids.push_back(texcoord_id);
    //} else {
    // this->texcoord_ids.push_back(iter->second);
    //}
  }
}

// Set the final height and scale the texcoords by removing
// redundant space
void IsaacTextureAtlas::resize_atlas() {
  // Round up a little to make it a multiple of 2 * padding. Not strictly necessary
  // but looks nicer that way.
  this->determined_height = (2 * padding) * (ceil(this->determined_height / static_cast<double>(2 * padding)));
  this->determined_height = std::min(this->height, this->determined_height);

  this->height = this->determined_height;
  this->image->resize(this->width, this->height, image->channels());
}

// Scale the texcoords once the final atlas dimensions are known
void IsaacTextureAtlas::scale_texcoords() {
  for (size_t tex_it = 0; tex_it < this->texcoords.size(); tex_it++) {
    texcoords[tex_it][0] /= this->width;
    texcoords[tex_it][1] /= this->height;
  }
}

void IsaacTextureAtlas::finalize() {
  if (finalized) throw util::Exception("IsaacTextureAtlas already finalized");

  this->bin.reset();
  this->resize_atlas();

  this->finalized = true;
}

void isaac_build_model(mve::TriangleMesh::ConstPtr mesh, std::vector<IsaacTextureAtlas::Ptr> const& texture_atlases,
                       ObjModel* obj_model) {
  mve::TriangleMesh::VertexList const& mesh_vertices = mesh->get_vertices();
  mve::TriangleMesh::NormalList const& mesh_normals = mesh->get_vertex_normals();
  mve::TriangleMesh::FaceList const& mesh_faces = mesh->get_faces();

  ObjModel::Vertices& vertices = obj_model->get_vertices();
  vertices.insert(vertices.begin(), mesh_vertices.begin(), mesh_vertices.end());
  ObjModel::Normals& normals = obj_model->get_normals();
  normals.insert(normals.begin(), mesh_normals.begin(), mesh_normals.end());
  ObjModel::TexCoords& texcoords = obj_model->get_texcoords();

  ObjModel::Groups& groups = obj_model->get_groups();
  MaterialLib& material_lib = obj_model->get_material_lib();

  for (IsaacTextureAtlas::Ptr texture_atlas : texture_atlases) {
    Material material;
    const std::size_t n = material_lib.size();
    material.name = std::string("material") + util::string::get_filled(n, 4);
    material.diffuse_map = texture_atlas->get_image();
    material_lib.push_back(material);

    groups.push_back(ObjModel::Group());
    ObjModel::Group& group = groups.back();
    group.material_name = material.name;

    IsaacTextureAtlas::Faces const& atlas_faces = texture_atlas->get_faces();
    IsaacTextureAtlas::Texcoords const& atlas_texcoords = texture_atlas->get_texcoords();
    IsaacTextureAtlas::TexcoordIds const& atlas_texcoord_ids = texture_atlas->get_texcoord_ids();

    std::size_t texcoord_id_offset = texcoords.size();

    texcoords.insert(texcoords.end(), atlas_texcoords.begin(), atlas_texcoords.end());

    for (std::size_t i = 0; i < atlas_faces.size(); ++i) {
      std::size_t mesh_face_pos = atlas_faces[i] * 3;

      std::size_t vertex_ids[] = {mesh_faces[mesh_face_pos], mesh_faces[mesh_face_pos + 1],
                                  mesh_faces[mesh_face_pos + 2]};
      std::size_t* normal_ids = vertex_ids;

      std::size_t texcoord_ids[] = {texcoord_id_offset + atlas_texcoord_ids[i * 3],
                                    texcoord_id_offset + atlas_texcoord_ids[i * 3 + 1],
                                    texcoord_id_offset + atlas_texcoord_ids[i * 3 + 2]};

      group.faces.push_back(ObjModel::Face());
      ObjModel::Face& face = group.faces.back();
      std::copy(vertex_ids, vertex_ids + 3, face.vertex_ids);
      std::copy(texcoord_ids, texcoord_ids + 3, face.texcoord_ids);
      std::copy(normal_ids, normal_ids + 3, face.normal_ids);
    }
  }
  // TODO(Oleg): remove unreferenced vertices/normals.
}

void loadMeshBuildTree(std::string const& mesh_file, mve::TriangleMesh::Ptr& mesh,
                       std::shared_ptr<mve::MeshInfo>& mesh_info, std::shared_ptr<tex::Graph>& graph,
                       std::shared_ptr<BVHTree>& bvh_tree) {
  std::cout << "Load and prepare mesh from " << mesh_file << std::endl;
  mesh = mve::geom::load_ply_mesh(mesh_file);

  mesh_info = std::shared_ptr<mve::MeshInfo>(new mve::MeshInfo(mesh));
  tex::prepare_mesh(mesh_info.get(), mesh);

  std::cout << "Building adjacency graph: " << std::endl;
  std::size_t const num_faces = mesh->get_faces().size() / 3;
  if (num_faces > std::numeric_limits<std::uint32_t>::max())
    throw std::runtime_error("Exeeded maximal number of faces");

  // TODO(oalexan1): Is this necessary?
  graph = std::shared_ptr<tex::Graph>(new tex::Graph(num_faces));
  tex::build_adjacency_graph(mesh, *mesh_info.get(), graph.get());

  util::WallTimer timer;
  std::vector<unsigned int> const& faces = mesh->get_faces();
  std::vector<math::Vec3f> const& vertices = mesh->get_vertices();
  std::cout << "Building BVH tree from " << faces.size() / 3 << " faces.\n";
  bvh_tree = std::shared_ptr<BVHTree>(new BVHTree(faces, vertices));
  std::cout << "Building the tree took: " << timer.get_elapsed() / 1000.0 << " seconds\n";
}

Eigen::Vector3d vec3f_to_eigen(math::Vec3f const& v) {
  Eigen::Vector3d V;
  for (size_t it = 0; it < 3; it++) V[it] = v[it];
  return V;
}

math::Vec3f eigen_to_vec3f(Eigen::Vector3d const& V) {
  math::Vec3f v;
  for (size_t it = 0; it < 3; it++) v[it] = V[it];
  return v;
}

void calculate_texture_size(double height_factor, std::list<IsaacTexturePatch::ConstPtr> const& texture_patches,
                            int& texture_width, int& texture_height) {
  int64_t total_area = 0;
  int max_width = 0;
  int max_height = 0;
  int padding = TILE_PADDING;

  for (IsaacTexturePatch::ConstPtr texture_patch : texture_patches) {
    int width = texture_patch->get_width() + 2 * padding;
    int height = texture_patch->get_height() + 2 * padding;

    max_width = std::max(max_width, width);
    max_height = std::max(max_height, height);

    int64_t area = width * height;
    total_area += area;
  }

  texture_width = std::max(4 * 1024, max_width);
  texture_height = ceil(static_cast<double>(total_area / texture_width));
  texture_height = std::max(texture_height, max_height);

  // This is a heuristic but it works well-enough the first time
  texture_height *= height_factor;
}

bool texture_patch_compare(IsaacTexturePatch::ConstPtr first, IsaacTexturePatch::ConstPtr second) {
  return first->get_size() > second->get_size();
}

// If there is more than one texture atlas merge them, as it is
// difficult to later manage multiple texture images.
// TODO(oalexan1): This does not work, despite trying to debug it a lot.
void merge_texture_atlases(std::vector<IsaacTextureAtlas::Ptr>* texture_atlases) {
  if (texture_atlases->size() <= 1) return;

  std::vector<IsaacTextureAtlas::Ptr> prev_atlases;
  prev_atlases.swap(*texture_atlases);

  // All atlases must have the same width to be merged
  for (size_t atlas_it = 0; atlas_it < prev_atlases.size(); atlas_it++) {
    if (prev_atlases[0]->get_width() != prev_atlases[atlas_it]->get_width())
      throw util::Exception("The atlases to merge do not have the same width.");
  }

  // Find the final width and height
  int final_height = 0, final_width = prev_atlases[0]->get_width();
  for (size_t atlas_it = 0; atlas_it < prev_atlases.size(); atlas_it++) {
    final_height += prev_atlases[atlas_it]->get_height();
  }

  // Create the final atlas. It already has enough space for the images,
  // but no texcoords exist yet
  texture_atlases->push_back(IsaacTextureAtlas::create(final_width, final_height));

  // Copy the image data
  int image_start = 0;
  mve::ByteImage::Ptr& dest_image = (*texture_atlases)[0]->get_image();
  for (size_t atlas_it = 0; atlas_it < prev_atlases.size(); atlas_it++) {
    mve::ByteImage::Ptr& prev_image = prev_atlases[atlas_it]->get_image();
    int prev_image_len = prev_image->width() * prev_image->height() * prev_image->channels();
    std::memcpy(dest_image->begin() + image_start, prev_image->begin(), prev_image_len);

    // prepare for the next iteration
    image_start += prev_image_len;
  }

  // Copy the texcoords
  int texcoord_start = 0;
  IsaacTextureAtlas::Texcoords& dest_coords = (*texture_atlases)[0]->get_texcoords();
  dest_coords.clear();
  for (size_t atlas_it = 0; atlas_it < prev_atlases.size(); atlas_it++) {
    IsaacTextureAtlas::Texcoords& prev_coords = prev_atlases[atlas_it]->get_texcoords();
    math::Vec2f offset = math::Vec2f(0, texcoord_start);

    IsaacTextureAtlas::Faces& prev_faces = prev_atlases[atlas_it]->get_faces();
    for (size_t face_it = 0; face_it < prev_faces.size(); face_it++) {
      for (int j = 0; j < 3; ++j) {
        math::Vec2f rel_texcoord(prev_coords[face_it * 3 + j]);
        math::Vec2f texcoord = rel_texcoord + offset;
        dest_coords.push_back(texcoord);
      }
    }

    // prepare for the next iteration
    texcoord_start += prev_atlases[atlas_it]->get_height();
  }

  // copy the faces
  IsaacTextureAtlas::Faces& dest_faces = (*texture_atlases)[0]->get_faces();
  dest_faces.clear();
  for (size_t atlas_it = 0; atlas_it < prev_atlases.size(); atlas_it++) {
    IsaacTextureAtlas::Faces& prev_faces = prev_atlases[atlas_it]->get_faces();
    for (size_t face_it = 0; face_it < prev_faces.size(); face_it++) {
      dest_faces.push_back(prev_faces[face_it]);
    }
  }

  prev_atlases.clear();  // wipe this
}

void generate_texture_atlases(double height_factor, std::vector<IsaacTexturePatch::ConstPtr>& texture_patches,
                              std::map<int, int>& face_positions, std::vector<IsaacTextureAtlas::Ptr>* texture_atlases,
                              std::vector<std::pair<int, int>>& atlas_sizes) {
  texture_atlases->clear();
  atlas_sizes.clear();

  // Make a copy of the pointers to texture patches that we will
  // modify Use a list from which it is easier to erase things.
  std::list<IsaacTexturePatch::ConstPtr> local_texture_patches;
  for (size_t it = 0; it < texture_patches.size(); it++) local_texture_patches.push_back(texture_patches[it]);

  /* Improve the bin-packing algorithm efficiency by sorting texture patches
   * in descending order of size. */
  local_texture_patches.sort(texture_patch_compare);

  // Find the face positions after sorting
  int count = 0;
  for (auto it = local_texture_patches.begin(); it != local_texture_patches.end(); it++) {
    IsaacTexturePatch::ConstPtr ptr = *it;
    IsaacTexturePatch const* P = ptr.get();
    int label = P->get_label();
    face_positions[label] = count;
    count++;
  }

  std::size_t const total_num_patches = local_texture_patches.size();
  std::size_t remaining_patches = local_texture_patches.size();

  while (!local_texture_patches.empty()) {
    int texture_width, texture_height;
    calculate_texture_size(height_factor, local_texture_patches, texture_width, texture_height);

    texture_atlases->push_back(IsaacTextureAtlas::create(texture_width, texture_height));
    IsaacTextureAtlas::Ptr texture_atlas = texture_atlases->back();

    /* Try to insert each of the texture patches into the texture atlas. */
    std::list<IsaacTexturePatch::ConstPtr>::iterator it = local_texture_patches.begin();
    for (; it != local_texture_patches.end();) {
      std::size_t done_patches = total_num_patches - remaining_patches;
      int precent = static_cast<float>(done_patches) / total_num_patches * 100.0f;
      if (total_num_patches > 100 && done_patches % (total_num_patches / 100) == 0) {
      }

      if (texture_atlas->insert(*it)) {
        it = local_texture_patches.erase(it);
        remaining_patches -= 1;
      } else {
        ++it;
      }
    }

    texture_atlas->finalize();  // this will change the atlas dimensions
  }

  // Merge into one single atlas
  // This does not work
  // merge_texture_atlases(texture_atlases);

  // Final tasks
  for (size_t atlas_it = 0; atlas_it < (*texture_atlases).size(); atlas_it++) {
    (*texture_atlases)[atlas_it]->merge_texcoords();
    (*texture_atlases)[atlas_it]->scale_texcoords();
    atlas_sizes.push_back(
      std::make_pair((*texture_atlases)[atlas_it]->get_width(), (*texture_atlases)[atlas_it]->get_height()));
  }

  return;
}

void isaac_save_model(ObjModel* obj_model, std::string const& prefix) {
  int OBJ_INDEX_OFFSET = 1;

  ObjModel::Vertices const& vertices = obj_model->get_vertices();
  ObjModel::Normals const& normals = obj_model->get_normals();
  ObjModel::TexCoords const& texcoords = obj_model->get_texcoords();
  ObjModel::Groups const& groups = obj_model->get_groups();
  MaterialLib const& material_lib = obj_model->get_material_lib();

  material_lib.save_to_files(prefix);

  std::string name = util::fs::basename(prefix);
  std::ofstream out((prefix + ".obj").c_str());
  if (!out.good()) throw util::FileException(prefix + ".obj", std::strerror(errno));

  out << "mtllib " << name << ".mtl" << '\n';

  out << std::fixed << std::setprecision(8);
  for (std::size_t i = 0; i < vertices.size(); ++i) {
    out << "v " << vertices[i][0] << " " << vertices[i][1] << " " << vertices[i][2] << '\n';
  }

  for (std::size_t i = 0; i < texcoords.size(); ++i) {
    out << "vt " << texcoords[i][0] << " " << 1.0f - texcoords[i][1] << '\n';
  }

  for (std::size_t i = 0; i < normals.size(); ++i) {
    out << "vn " << normals[i][0] << " " << normals[i][1] << " " << normals[i][2] << '\n';
  }

  for (std::size_t i = 0; i < groups.size(); ++i) {
    out << "usemtl " << groups[i].material_name << '\n';
    for (std::size_t j = 0; j < groups[i].faces.size(); ++j) {
      ObjModel::Face const& face = groups[i].faces[j];
      out << "f";
      for (std::size_t k = 0; k < 3; ++k) {
        out << " " << face.vertex_ids[k] + OBJ_INDEX_OFFSET << "/" << face.texcoord_ids[k] + OBJ_INDEX_OFFSET << "/"
            << face.normal_ids[k] + OBJ_INDEX_OFFSET;
      }
      out << '\n';
    }
  }
  out.close();
}

typedef std::pair<double, double> pointPair;

// Find the intersection of two lines. Return true if the intersection succeeded.
bool edge_intersection(pointPair A, pointPair B, pointPair C, pointPair D, pointPair& out) {
  // Line AB represented as a1 * x + b1 * y = c1
  double a = B.second - A.second;
  double b = A.first - B.first;
  double c = a * (A.first) + b * (A.second);
  // Line CD represented as a2 * x + b2 * y = c2
  double a1 = D.second - C.second;
  double b1 = C.first - D.first;
  double c1 = a1 * (C.first) + b1 * (C.second);
  double det = a * b1 - a1 * b;
  if (det == 0) {
    return false;
  } else {
    double x = (b1 * c - b * c1) / det;
    double y = (a * c1 - a1 * c) / det;
    out = std::make_pair(x, y);
  }

  return true;
}

enum EdgeType { right_edge_normal, left_edge_normal, horizontal_edge_normal };

// A simple edge structure
struct Edge {
  Edge() {}
  Edge(double x0, double y0, double x1, double y1) : x0(x0), y0(y0), x1(x1), y1(y1) {
    // See if the normal to the edge points right, left, or up/down
    // (when the edge is horizontal)

    if (y1 < y0) {
      edge_type = left_edge_normal;
    } else if (y1 > y0) {
      edge_type = right_edge_normal;
    } else {
      edge_type = horizontal_edge_normal;
    }
  }

  pointPair get_normal() const {
    double a = (y1 - y0);
    double b = -(x1 - x0);
    double len = std::sqrt(a * a + b * b);
    if (len != 0) {
      a /= len;
      b /= len;
    }
    return std::make_pair(a, b);
  }

  // Intersect an edge with a horizontal line at height x. Return the
  // y endpoint of that intersection. Return false if the edge is horizontal as well.
  bool horizontal_line_intersect(double y, double& out_x) {
    if (y < y0 && y < y1) return false;
    if (y > y0 && y > y1) return false;
    if (edge_type == horizontal_edge_normal) {
      return false;
    }

    out_x = (y - y0) * (x1 - x0) / (y1 - y0) + x0;

    return true;
  }

  double x0, y0, x1, y1;
  EdgeType edge_type;
};

// Move an edge this far along the edge normal
Edge bias_edge(Edge const& e, double bias) {
  pointPair n = e.get_normal();
  return Edge(e.x0 + bias * n.first, e.y0 + bias * n.second, e.x1 + bias * n.first, e.y1 + bias * n.second);
}

bool edge_intersection(Edge A, Edge B, pointPair& out) {
  return edge_intersection(std::make_pair(A.x0, A.y0), std::make_pair(A.x1, A.y1), std::make_pair(B.x0, B.y0),
                           std::make_pair(B.x1, B.y1), out);
}

// A triangle with three edges
struct Triangle {
  Triangle(double x0, double y0, double x1, double y1, double x2, double y2)
      : x0(x0), y0(y0), x1(x1), y1(y1), x2(x2), y2(y2) {
    E[0] = Edge(x0, y0, x1, y1);
    E[1] = Edge(x1, y1, x2, y2);
    E[2] = Edge(x2, y2, x0, y0);
  }

  // Intersect a triangle with a horizontal line at height x. Return the
  // y endpoints of that intersection, if successful
  bool horizontal_line_intersect(double y, double& out_x0, double& out_x1) {
    out_x0 = -std::numeric_limits<double>::max();
    out_x1 = std::numeric_limits<double>::max();
    bool success = false;
    for (size_t it = 0; it < 3; it++) {
      double out_x;
      bool ans = E[it].horizontal_line_intersect(y, out_x);
      if (!ans) continue;

      success = true;

      if (E[it].edge_type == left_edge_normal) {
        if (out_x0 < out_x) out_x0 = out_x;
      } else if (E[it].edge_type == right_edge_normal) {
        if (out_x1 > out_x) out_x1 = out_x;
      }
    }

    return success;
  }

  Edge E[3];
  double x0, y0, x1, y1, x2, y2;
};

// Bias each triangle edge by a given amount and compute the new points
// of intersection
Triangle bias_triangle(Triangle const& tri, double bias) {
  Edge biased_edges[3];

  for (size_t it = 0; it < 3; it++) biased_edges[it] = bias_edge(tri.E[it], bias);

  // Find the intersections
  pointPair E0, E1, E2;
  bool ans0, ans1, ans2;

  ans0 = edge_intersection(biased_edges[0], biased_edges[2], E0);
  ans1 = edge_intersection(biased_edges[0], biased_edges[1], E1);
  ans2 = edge_intersection(biased_edges[1], biased_edges[2], E2);

  // Apply the bias only for non-degenerate triangles
  if (ans0 && ans1 && ans2) return Triangle(E0.first, E0.second, E1.first, E1.second, E2.first, E2.second);

  return tri;
}

// Given a mesh and a given pixel size in meters (say 0.001),
// overlay a grid of that pixel size on top of each triangle and
// form a textured 3D model with a pixel at each node.
// Store the transforms that allows one at any time to
// project an image onto that triangle and copy the image
// interpolated at the grid points into the texture.
// This way the size of the texture image is fixed
// for the given texture mesh and different images result
// in different pixels in this texture.

void formModel(mve::TriangleMesh::ConstPtr mesh, double pixel_size, int num_threads,
               // outputs
               std::vector<FaceInfo>& face_projection_info, std::vector<IsaacTextureAtlas::Ptr>& texture_atlases,
               tex::Model& model) {
  omp_set_dynamic(0);                // Explicitly disable dynamic teams
  omp_set_num_threads(num_threads);  // Use this many threads for all
                                     // consecutive parallel regions

  std::cout << "Forming the textured model" << std::endl;
  util::WallTimer timer;

  std::vector<math::Vec3f> const& vertices = mesh->get_vertices();
  std::vector<math::Vec3f> const& mesh_normals = mesh->get_vertex_normals();
  if (vertices.size() != mesh_normals.size()) LOG(FATAL) << "A mesh must have as many vertices as vertex normals.";

  std::vector<unsigned int> const& faces = mesh->get_faces();
  std::vector<math::Vec3f> const& face_normals = mesh->get_face_normals();

  int num_faces = faces.size() / 3;
  face_projection_info.resize(num_faces);

  // Store here texture coords before they get packed in the model
  ObjModel::TexCoords input_texcoords(3 * num_faces);

  std::vector<IsaacTexturePatch::ConstPtr> texture_patches(num_faces);

#pragma omp parallel for
  for (int face_id = 0; face_id < num_faces; face_id++) {
    math::Vec3f const& v1 = vertices[faces[3 * face_id + 0]];
    math::Vec3f const& v2 = vertices[faces[3 * face_id + 1]];
    math::Vec3f const& v3 = vertices[faces[3 * face_id + 2]];
    // math::Vec3f const & face_normal = face_normals[face_id];

    // A mesh triangle is visible if rays from its vertices do not
    // intersect the mesh somewhere else before hitting the camera,
    // and hit the camera inside the image bounds.
    math::Vec3f const* samples[] = {&v1, &v2, &v3};

    std::vector<std::size_t> patch_faces(1);
    patch_faces[0] = face_id;

    // Convert the triangle corners to Eigen so that we have double precision
    std::vector<Eigen::Vector3d> V(3);
    for (size_t v_it = 0; v_it < 3; v_it++) V[v_it] = vec3f_to_eigen(*samples[v_it]);

    // Compute the double precision normal, as we will do a lot of
    // careful math. It agrees with the pre-existing float normal.
    Eigen::Vector3d N = (V[1] - V[0]).cross(V[2] - V[0]);
    if (N != Eigen::Vector3d(0, 0, 0)) N.normalize();
    // Eigen::Vector3d N0 = vec3f_to_eigen(face_normal); // pre-existing
    // normal

    double a, e;
    dense_map::normalToAzimuthAndElevation(N, a, e);

    // TODO(oalexan1): This needs to be a function
    // Find the transform YZPlaneToTriangleFace which maps (1, 0, 0) to N
    Eigen::Matrix3d A;
    A << cos(a), -sin(a), 0, sin(a), cos(a), 0, 0, 0, 1;
    Eigen::Matrix3d E;
    E << cos(e), 0, -sin(e), 0, 1, 0, sin(e), 0, cos(e);
    Eigen::Affine3d YZPlaneToTriangleFace;
    YZPlaneToTriangleFace.linear() = A * E;
    YZPlaneToTriangleFace.translation() = Eigen::Vector3d(0, 0, 0);
    Eigen::Affine3d inv_trans = YZPlaneToTriangleFace.inverse();

    // Transform the vertices, they will end up in the same plane
    // with x constant.
    std::vector<Eigen::Vector3d> TransformedVertices;
    TransformedVertices.resize(3);
    double x = 0, min_y = 0, max_y = 0, min_z = 0, max_z = 0;
    for (size_t v_it = 0; v_it < 3; v_it++) {
      TransformedVertices[v_it] = inv_trans * V[v_it];
      if (v_it == 0) {
        x = TransformedVertices[v_it][0];
        min_y = TransformedVertices[v_it][1];
        max_y = TransformedVertices[v_it][1];
        min_z = TransformedVertices[v_it][2];
        max_z = TransformedVertices[v_it][2];
      } else {
        min_y = std::min(min_y, TransformedVertices[v_it][1]);
        max_y = std::max(max_y, TransformedVertices[v_it][1]);
        min_z = std::min(min_z, TransformedVertices[v_it][2]);
        max_z = std::max(max_z, TransformedVertices[v_it][2]);
      }
    }

    // Put a little padding just in case
    // TODO(oalexan1): Put back a padding of 2 maybe!
    int padding = 1;
    min_y -= padding * pixel_size;
    max_y += padding * pixel_size;
    min_z -= padding * pixel_size;
    max_z += padding * pixel_size;

    int width = ceil((max_y - min_y) / pixel_size) + 1;
    int height = ceil((max_z - min_z) / pixel_size) + 1;

    // std::cout << "width and height are " << width << ' ' << height <<
    // std::endl;

    std::vector<math::Vec2f> face_texcoords(3);

    // Record where the triangle vertices will be in the sampled bounding box of the face
    for (size_t v_it = 0; v_it < 3; v_it++) {
      face_texcoords[v_it] = math::Vec2f((TransformedVertices[v_it][1] - min_y) / pixel_size,
                                         (TransformedVertices[v_it][2] - min_z) / pixel_size);
    }

    // Append these to the bigger list
    for (size_t v_it = 0; v_it < face_texcoords.size(); v_it++) {
      input_texcoords[3 * face_id + v_it] = face_texcoords[v_it];
    }

    // Store the info that we will need later to find where a little tile
    // should go.
    FaceInfo& F = face_projection_info[face_id];  // alias
    F.x = x;
    F.min_y = min_y;
    F.min_z = min_z;
    F.width = width;
    F.height = height;
    F.TransformedVertices = TransformedVertices;
    F.YZPlaneToTriangleFace = YZPlaneToTriangleFace;
    F.padding = padding;

    // We won't really use this image, but need its dimensions
    texture_patches[face_id] = IsaacTexturePatch::create(face_id, patch_faces, face_texcoords, width, height);
  }  // End loop over mesh faces

  // for face i, 3 * face_position[i] will be where it is starts being stored
  // in the uv array.
  std::map<int, int> face_positions;
  std::vector<std::pair<int, int>> atlas_sizes;
  double height_factor = 2.0;

  // It is a pain to deal with multiple texture images, which result from
  // multiple atlases. If that happens, just try again with bigger space
  // allocated for the atlas. Normally we will get it right from the first
  // try though.
  for (int attempt = 0; attempt < 10; attempt++) {
    generate_texture_atlases(height_factor, texture_patches, face_positions, &texture_atlases, atlas_sizes);

    if (texture_atlases.size() <= 1) break;

    std::cout << "More than one texture atlas created. Try to create instead "
              << "a single atlas of larger dimensions. This is attempt: " << attempt << ".\n";
    height_factor += 1.0;
  }

  // If still failed, give up
  if (texture_atlases.size() > 1) LOG(FATAL) << "More than one texture atlas was generated, which is not supported.";

  if (texture_atlases.empty()) LOG(FATAL) << "No texture atlas was created.";

  int texture_width = atlas_sizes[0].first;
  int texture_height = atlas_sizes[0].second;

  // Build the model from atlases
  isaac_build_model(mesh, texture_atlases, &model);

  // These texcoords and the ones above are related by a scale transform + translation
  // that transform that can change depending on which atlas we are on.
  ObjModel::TexCoords const& model_texcoords = model.get_texcoords();

  // The two vectors below would be equal if we process the mesh fully,
  // and not so unless we stop early.
  if (model_texcoords.size() != input_texcoords.size()) LOG(FATAL) << "Book-keeping failure regarding texcoords.";

  for (std::size_t i = 0; i < model_texcoords.size() / 3; i++) {
    auto face_pos = face_positions.find(i);
    if (face_pos == face_positions.end()) {
      LOG(FATAL) << "Cannot find position for index " << i;
    }

    int mapped_i = face_pos->second;

    // By comparing where the texcoords were before being put in the
    // textured mesh file and where it ends up in that file, we can
    // determine the shift that was used to place a texture patch in
    // the texture.
    int shift_u = round(model_texcoords[3 * mapped_i][0] * texture_width - input_texcoords[3 * i][0]);
    int shift_v = round(model_texcoords[3 * mapped_i][1] * texture_height - input_texcoords[3 * i][1]);

    face_projection_info[i].shift_u = shift_u;
    face_projection_info[i].shift_v = shift_v;
  }

  std::cout << "Forming the model took: " << timer.get_elapsed() / 1000.0 << " seconds\n";
}

void formMtl(std::string const& out_prefix, std::string& mtl_str) {
  std::ostringstream ofs;
  ofs << "newmtl material0000\n";
  ofs << "Ka 0.200000 0.200000 0.200000\n";
  ofs << "Kd 1.000000 1.000000 1.000000\n";
  ofs << "Ks 1.000000 1.000000 1.000000\n";
  ofs << "Tr 0.000000\n";
  ofs << "illum 2\n";
  ofs << "Ns 0.000000\n";
  ofs << "map_Kd " << out_prefix << ".png\n";
  mtl_str = ofs.str();

  std::string mtl_file = out_prefix + ".mtl";
  std::ofstream ofs2(mtl_file.c_str());
  ofs2 << mtl_str;
  ofs2.close();
}

void formObjCustomUV(mve::TriangleMesh::ConstPtr mesh, std::vector<Eigen::Vector3i> const& face_vec,
                     std::map<int, Eigen::Vector2d> const& uv_map, std::string const& out_prefix,
                     std::string& obj_str) {
  // Get handles to the vertices and vertex normals
  std::vector<math::Vec3f> const& vertices = mesh->get_vertices();
  std::vector<math::Vec3f> const& mesh_normals = mesh->get_vertex_normals();
  if (vertices.size() != mesh_normals.size()) LOG(FATAL) << "A mesh must have as many vertices as vertex normals.";

  // The uv_map assigns to each of the mesh vertices that is visible
  // in the texture its (u, v) pair. Find the map from each vertex
  // index to the index in the list of (u, v) pairs.
  std::map<int, int> vertex_to_uv;
  int count = 0;
  for (auto it = uv_map.begin(); it != uv_map.end(); it++) {
    vertex_to_uv[it->first] = count;
    count++;
  }

  std::ostringstream out;

  out << "mtllib " << out_prefix << ".mtl\n";

  out << std::fixed << std::setprecision(8);
  for (std::size_t i = 0; i < vertices.size(); i++) {
    out << "v " << vertices[i][0] << " " << vertices[i][1] << " " << vertices[i][2] << '\n';
  }

  for (auto it = uv_map.begin(); it != uv_map.end(); it++) {
    out << "vt " << (it->second)[0] << " " << (it->second)[1] << '\n';
  }

  for (std::size_t i = 0; i < mesh_normals.size(); i++) {
    out << "vn " << mesh_normals[i][0] << " " << mesh_normals[i][1] << " " << mesh_normals[i][2] << '\n';
  }

  int OBJ_INDEX_OFFSET = 1;  // have indices start from 1
  for (std::size_t j = 0; j < face_vec.size(); j++) {
    out << "f";
    for (std::size_t k = 0; k < 3; ++k) {
      out << " " << face_vec[j][k] + OBJ_INDEX_OFFSET << "/" << vertex_to_uv[face_vec[j][k]] + OBJ_INDEX_OFFSET << "/"
          << face_vec[j][k] + OBJ_INDEX_OFFSET;
    }
    out << '\n';
  }
  obj_str = out.str();
}

void formObj(tex::Model& texture_model, std::string const& out_prefix, std::string& obj_str) {
  std::vector<math::Vec3f> const& vertices = texture_model.get_vertices();
  std::vector<math::Vec2f> const& texcoords = texture_model.get_texcoords();
  std::vector<math::Vec3f> const& normals = texture_model.get_normals();
  std::vector<ObjModel::Group> const& groups = texture_model.get_groups();

  std::ostringstream out;

  out << "mtllib " << out_prefix << ".mtl\n";

  out << std::fixed << std::setprecision(8);
  for (std::size_t i = 0; i < vertices.size(); i++) {
    out << "v " << vertices[i][0] << " " << vertices[i][1] << " " << vertices[i][2] << '\n';
  }

  for (std::size_t i = 0; i < texcoords.size(); ++i) {
    out << "vt " << texcoords[i][0] << " " << 1.0f - texcoords[i][1] << '\n';
  }

  for (std::size_t i = 0; i < normals.size(); i++) {
    out << "vn " << normals[i][0] << " " << normals[i][1] << " " << normals[i][2] << '\n';
  }

  int OBJ_INDEX_OFFSET = 1;  // have indices start from 1

  for (std::size_t i = 0; i < groups.size(); ++i) {
    out << "usemtl " << groups[i].material_name << '\n';
    for (std::size_t j = 0; j < groups[i].faces.size(); ++j) {
      ObjModel::Face const& face = groups[i].faces[j];
      out << "f";
      for (std::size_t k = 0; k < 3; ++k) {
        out << " " << face.vertex_ids[k] + OBJ_INDEX_OFFSET << "/" << face.texcoord_ids[k] + OBJ_INDEX_OFFSET << "/"
            << face.normal_ids[k] + OBJ_INDEX_OFFSET;
      }
      out << '\n';
    }
  }

  obj_str = out.str();
}

// Project texture and find the UV coordinates
void projectTexture(mve::TriangleMesh::ConstPtr mesh, std::shared_ptr<BVHTree> bvh_tree, cv::Mat const& image,
                    camera::CameraModel const& cam, double num_exclude_boundary_pixels,
                    // outputs
                    std::vector<double>& smallest_cost_per_face, std::vector<Eigen::Vector3i>& face_vec,
                    std::map<int, Eigen::Vector2d>& uv_map) {
  // Wipe the outputs
  face_vec.clear();
  uv_map.clear();

  // Here need to take into account that for real (not simulated)
  // images the camera may have been calibrated at 1/4 the original
  // image resolution. We will use the calibrated dimensions when
  // normalizing u and v, because when projecting 3D points in the
  // camera we will the calibrated camera model.
  int raw_image_cols = image.cols;
  int raw_image_rows = image.rows;
  int calib_image_cols = cam.GetParameters().GetDistortedSize()[0];
  int calib_image_rows = cam.GetParameters().GetDistortedSize()[1];

  int factor = raw_image_cols / calib_image_cols;

  if ((raw_image_cols != calib_image_cols * factor) || (raw_image_rows != calib_image_rows * factor)) {
    LOG(FATAL) << "Published image width and height are: " << raw_image_cols << ' ' << raw_image_rows << "\n"
               << "Calibrated image width and height are: " << calib_image_cols << ' ' << calib_image_rows << "\n"
               << "These must be equal up to an integer factor.\n";
  }

  // To compensate for calibration being done at lower resolution
  num_exclude_boundary_pixels /= static_cast<double>(factor);

  Eigen::Vector3d cam_ctr = cam.GetPosition();

  std::vector<math::Vec3f> const& vertices = mesh->get_vertices();
  std::vector<math::Vec3f> const& mesh_normals = mesh->get_vertex_normals();
  if (vertices.size() != mesh_normals.size()) LOG(FATAL) << "A mesh must have as many vertices as vertex normals.";

  std::vector<unsigned int> const& faces = mesh->get_faces();
  std::vector<math::Vec3f> const& face_normals = mesh->get_face_normals();

  if (smallest_cost_per_face.size() != faces.size()) LOG(FATAL) << "There must be one cost value per face.";

#pragma omp parallel for
  for (std::size_t face_id = 0; face_id < faces.size() / 3; face_id++) {
    math::Vec3f const& v1 = vertices[faces[3 * face_id + 0]];
    math::Vec3f const& v2 = vertices[faces[3 * face_id + 1]];
    math::Vec3f const& v3 = vertices[faces[3 * face_id + 2]];
    math::Vec3f const& face_normal = face_normals[face_id];
    math::Vec3f const face_center = (v1 + v2 + v3) / 3.0f;

    // Do some geometric checks and compute the cost for this face and camera

    Eigen::Vector3d cam_to_face_vec = (vec3f_to_eigen(face_center) - cam_ctr).normalized();
    Eigen::Vector3d face_to_cam_vec = -cam_to_face_vec;
    double face_normal_to_cam_dot_prod = face_to_cam_vec.dot(vec3f_to_eigen(face_normal));

    if (face_normal_to_cam_dot_prod <= 0.0) continue;  // The face points away from the camera

    // Angle between face normal and ray from face center to camera center
    // is bigger than 75 degrees.
    // TODO(oalexan1): Make this a parameter.
    // TODO(oalexan1): Filter by distance from each of
    // v1, v2, v3 to view_pos.
    double face_normal_to_cam_angle = std::acos(face_normal_to_cam_dot_prod);  // radians
    if (face_normal_to_cam_angle > 75.0 * M_PI / 180.0) continue;

    // The further a camera is and the bigger then angle between the
    // camera direction and the face normal, the less we want this
    // camera's texture for this triangle.
    double cost_val = face_normal_to_cam_angle + (vec3f_to_eigen(face_center) - cam_ctr).norm();
    if (cost_val >= smallest_cost_per_face[face_id]) {
      continue;
    }

    // A mesh triangle is visible if rays from its vertices do not
    // intersect the mesh somewhere else before hitting the camera,
    // and hit the camera inside the image bounds.
    bool visible = true;
    math::Vec3f const* samples[] = {&v1, &v2, &v3};

    std::vector<Eigen::Vector2d> UV;
    for (std::size_t vertex_it = 0; vertex_it < 3; vertex_it++) {
      BVHTree::Ray ray;
      ray.origin = *samples[vertex_it];
      ray.dir = eigen_to_vec3f(cam_ctr) - ray.origin;
      ray.tmax = ray.dir.norm();
      ray.tmin = ray.tmax * 0.0001f;
      ray.dir.normalize();

      BVHTree::Hit hit;
      if (bvh_tree->intersect(ray, &hit)) {
        visible = false;
        break;
      }

      // Triangle vertex in world coordinates
      Eigen::Vector3d world_pt = vec3f_to_eigen(*samples[vertex_it]);

      // Transform the vertex to camera coordinates
      Eigen::Affine3d const& world2cam = cam.GetTransform();
      Eigen::Vector3d cam_pt = world2cam * world_pt;

      // Skip points that project behind the camera
      if (cam_pt.z() <= 0) {
        visible = false;
        break;
      }

      // Get the undistorted pixel
      Eigen::Vector2d undist_centered_pix = cam.GetParameters().GetFocalVector().cwiseProduct(cam_pt.hnormalized());
      if (std::abs(undist_centered_pix[0]) > cam.GetParameters().GetUndistortedHalfSize()[0] ||
          std::abs(undist_centered_pix[1]) > cam.GetParameters().GetUndistortedHalfSize()[1]) {
        // If we are out of acceptable undistorted region, there's some uncertainty whether
        // the distortion computation in the next operation will work, so quit early.
        visible = false;
        break;
      }

      // Get the distorted pixel value
      Eigen::Vector2d dist_pix;
      cam.GetParameters().Convert<camera::UNDISTORTED_C, camera::DISTORTED>(undist_centered_pix, &dist_pix);

      // Skip pixels that don't project in the image
      if (dist_pix.x() < num_exclude_boundary_pixels ||
          dist_pix.x() > calib_image_cols - 1 - num_exclude_boundary_pixels ||
          dist_pix.y() < num_exclude_boundary_pixels ||
          dist_pix.y() > calib_image_rows - 1 - num_exclude_boundary_pixels) {
        visible = false;
        break;
      }

      // Find the u, v coordinates of each vertex
      double u = dist_pix.x() / calib_image_cols;
      // TODO(oalexan1): Maybe use:
      // v = (calib_image_rows - 1 - dist_pix.y())/image_rows ?
      double v = 1.0 - dist_pix.y() / calib_image_rows;

      UV.push_back(Eigen::Vector2d(u, v));
    }

    if (!visible) continue;

#pragma omp critical
    {
      // Here we integrate the results from each loop iteration so have
      // to use a lock (critical section).

      // Sanity check, to ensure nothing got mixed up with the multiple
      // threads
      if (cost_val >= smallest_cost_per_face[face_id]) LOG(FATAL) << "Book-keeping error in estimating cost per face.";

      smallest_cost_per_face[face_id] = cost_val;

      for (std::size_t vertex_it = 0; vertex_it < 3; vertex_it++)
        uv_map[faces[3 * face_id + vertex_it]] = UV[vertex_it];

      face_vec.push_back(Eigen::Vector3i(faces[3 * face_id + 0], faces[3 * face_id + 1], faces[3 * face_id + 2]));
    }
  }  // End loop over mesh faces
}

// Project texture using a texture model that was already pre-filled, so
// just update pixel values
void projectTexture(mve::TriangleMesh::ConstPtr mesh, std::shared_ptr<BVHTree> bvh_tree, cv::Mat const& image,
                    camera::CameraModel const& cam, std::vector<double>& smallest_cost_per_face, double pixel_size,
                    int num_threads, std::vector<FaceInfo> const& face_projection_info,
                    std::vector<IsaacTextureAtlas::Ptr>& texture_atlases, tex::Model& model, cv::Mat& out_texture) {
  omp_set_dynamic(0);                // Explicitly disable dynamic teams
  omp_set_num_threads(num_threads);  // Use this many threads for all
                                     // consecutive parallel regions

  util::WallTimer timer;

  // Here need to take into account that for real (not simulated)
  // images the camera may have been calibrated at 1/4 the original
  // image resolution. We will use the calibrated dimensions when
  // normalizing u and v, because when projecting 3D points in the
  // camera we will the calibrated camera model.
  int raw_image_cols = image.cols;
  int raw_image_rows = image.rows;
  int calib_image_cols = cam.GetParameters().GetDistortedSize()[0];
  int calib_image_rows = cam.GetParameters().GetDistortedSize()[1];

  int factor = raw_image_cols / calib_image_cols;
  if ((raw_image_cols != calib_image_cols * factor) || (raw_image_rows != calib_image_rows * factor)) {
    LOG(FATAL) << "Published image width and height are: " << raw_image_cols << ' ' << raw_image_rows << "\n"
               << "Calibrated image width and height are: " << calib_image_cols << ' ' << calib_image_rows << "\n"
               << "These must be equal up to an integer factor.\n";
  }

  if (texture_atlases.size() != 1) LOG(FATAL) << "Expecting just one texture in the model.";

  // Get a handle to the texture and make it unsigned
  mve::ByteImage::Ptr texture = texture_atlases[0]->get_image();
  unsigned char* texture_ptr = (unsigned char*)texture->get_byte_pointer();

  // Blank the image
  texture->fill(0);

  if (texture->channels() != NUM_CHANNELS) throw util::Exception("Wrong number of channels in the texture image.");

  Eigen::Vector3d cam_ctr = cam.GetPosition();

  std::vector<math::Vec3f> const& vertices = mesh->get_vertices();
  std::vector<math::Vec3f> const& mesh_normals = mesh->get_vertex_normals();
  if (vertices.size() != mesh_normals.size()) LOG(FATAL) << "A mesh must have as many vertices as vertex normals.";

  std::vector<unsigned int> const& faces = mesh->get_faces();
  std::vector<math::Vec3f> const& face_normals = mesh->get_face_normals();

  if (smallest_cost_per_face.size() != faces.size()) LOG(FATAL) << "There must be one cost value per face.";

#pragma omp parallel for
  for (std::size_t face_id = 0; face_id < faces.size() / 3; face_id++) {
    math::Vec3f const& v1 = vertices[faces[3 * face_id + 0]];
    math::Vec3f const& v2 = vertices[faces[3 * face_id + 1]];
    math::Vec3f const& v3 = vertices[faces[3 * face_id + 2]];
    math::Vec3f const& face_normal = face_normals[face_id];
    math::Vec3f const face_center = (v1 + v2 + v3) / 3.0f;

    // Do some geometric checks and compute the cost for this face and camera

    Eigen::Vector3d cam_to_face_vec = (vec3f_to_eigen(face_center) - cam_ctr).normalized();
    Eigen::Vector3d face_to_cam_vec = -cam_to_face_vec;
    double face_normal_to_cam_dot_prod = face_to_cam_vec.dot(vec3f_to_eigen(face_normal));

    if (face_normal_to_cam_dot_prod <= 0.0) continue;  // The face points away from the camera

    // Angle between face normal and ray from face center to camera center
    // is bigger than 75 degrees.
    // TODO(oalexan1): Make this a parameter.
    // TODO(oalexan1): Filter by distance from each of
    // v1, v2, v3 to view_pos.
    double face_normal_to_cam_angle = std::acos(face_normal_to_cam_dot_prod);  // radians
    if (face_normal_to_cam_angle > 75.0 * M_PI / 180.0) continue;

    // The further a camera is and the bigger then angle between the
    // camera direction and the face normal, the less we want this
    // camera's texture for this triangle.
    double cost_val = face_normal_to_cam_angle + (vec3f_to_eigen(face_center) - cam_ctr).norm();
    if (cost_val >= smallest_cost_per_face[face_id]) {
      continue;
    }

    // A mesh triangle is visible if rays from its vertices do not
    // intersect the mesh somewhere else before hitting the camera,
    // and hit the camera inside the image bounds.
    bool visible = true;
    math::Vec3f const* samples[] = {&v1, &v2, &v3};

    for (std::size_t vertex_it = 0; vertex_it < 3; vertex_it++) {
      BVHTree::Ray ray;
      ray.origin = *samples[vertex_it];
      ray.dir = eigen_to_vec3f(cam_ctr) - ray.origin;
      ray.tmax = ray.dir.norm();
      ray.tmin = ray.tmax * 0.0001f;
      ray.dir.normalize();

      BVHTree::Hit hit;
      if (bvh_tree->intersect(ray, &hit)) {
        visible = false;
        break;
      }

      // Triangle vertex in world coordinates
      Eigen::Vector3d world_pt = vec3f_to_eigen(*samples[vertex_it]);

      // Transform the vertex to camera coordinates
      Eigen::Affine3d const& world2cam = cam.GetTransform();
      Eigen::Vector3d cam_pt = world2cam * world_pt;

      // Skip points that project behind the camera
      if (cam_pt.z() <= 0) {
        visible = false;
        break;
      }

      // Get the undistorted pixel
      Eigen::Vector2d undist_centered_pix = cam.GetParameters().GetFocalVector().cwiseProduct(cam_pt.hnormalized());
      if (std::abs(undist_centered_pix[0]) > cam.GetParameters().GetUndistortedHalfSize()[0] ||
          std::abs(undist_centered_pix[1]) > cam.GetParameters().GetUndistortedHalfSize()[1]) {
        // If we are out of acceptable undistorted region, there's some uncertainty whether
        // the distortion computation in the next operation will work, so quit early.
        visible = false;
        break;
      }

      // Get the distorted pixel value
      Eigen::Vector2d dist_pix;
      cam.GetParameters().Convert<camera::UNDISTORTED_C, camera::DISTORTED>(undist_centered_pix, &dist_pix);

      // Skip pixels that don't project in the image
      if (dist_pix.x() < 0 || dist_pix.x() > calib_image_cols - 1 || dist_pix.y() < 0 ||
          dist_pix.y() > calib_image_rows - 1) {
        visible = false;
        break;
      }
    }

    // Skip faces that are not fully seen from this camera
    if (!visible) continue;

    FaceInfo const& F = face_projection_info[face_id];  // alias

    // Skip faces for which we did not figure out how to project them
    if (F.shift_u == std::numeric_limits<int>::max()) continue;

    // Form the transform from the y-z plane pixels to the camera
    Eigen::Affine3d const& world2cam = cam.GetTransform();
    Eigen::Affine3d const& YZPlaneToTriangleFace = F.YZPlaneToTriangleFace;

    // The transform from sampled pixel space to the y-z plane
    Eigen::Affine3d S;
    S.matrix() << 1, 0, 0, F.x, 0, pixel_size, 0, F.min_y, 0, 0, pixel_size, F.min_z, 0, 0, 0, 1;
    Eigen::Affine3d T = world2cam * YZPlaneToTriangleFace * S;

    // Sample the triangle face in predetermined fashion and
    // project all those samples in the camera.
    // Do not sample the entire padded bounding box of the triangle.
    // Rather, look only at samples within the triangle biased
    // by the padding
    std::vector<Eigen::Vector3d> const& TV = F.TransformedVertices;  // alias

    // The vertices of the face transformed to a y-z plane (ignore the x)
    Triangle tri(TV[0][1], TV[0][2], TV[1][1], TV[1][2], TV[2][1], TV[2][2]);

    // Apply a 1.5 * pixel_size bias * padding to ensure all the pixels around
    // the triangle are sampled well.
    tri = bias_triangle(tri, 1.5 * pixel_size * F.padding);

    for (int iz = 0; iz < F.height; iz++) {
      double out_y0, out_y1;
      double z = F.min_z + iz * pixel_size;
      bool success = tri.horizontal_line_intersect(z, out_y0, out_y1);

      if (!success) continue;

      int min_iy = std::max(static_cast<int>(floor((out_y0 - F.min_y) / pixel_size)), 0);
      int max_iy = std::min(static_cast<int>(ceil((out_y1 - F.min_y) / pixel_size)), F.width - 1);

      for (int iy = min_iy; iy <= max_iy; iy++) {
        // The point in the plane in which we do the book-keeping
        // Eigen::Vector3d P(F.x, F.min_y + iy * pixel_size, F.min_z + iz *
        // pixel_size);
        // Transform to the actual plane of the triangle
        // P = F.YZPlaneToTriangleFace * P;
        // Transform the vertex to camera coordinates
        // Eigen::Vector3d cam_pt = T * P;

        // Optimized version in the above
        Eigen::Vector3d cam_pt = T * Eigen::Vector3d(0, iy, iz);

        // Skip points that project behind the camera
        if (cam_pt.z() <= 0) continue;

        // Get the undistorted pixel
        Eigen::Vector2d undist_centered_pix = cam.GetParameters().GetFocalVector().cwiseProduct(cam_pt.hnormalized());
        if (std::abs(undist_centered_pix[0]) > cam.GetParameters().GetUndistortedHalfSize()[0] ||
            std::abs(undist_centered_pix[1]) > cam.GetParameters().GetUndistortedHalfSize()[1]) {
          // If we are out of acceptable undistorted region, there's some uncertainty whether
          // the distortion computation in the next operation will work, so quit early.
          visible = false;
          break;
        }

        // Get the distorted pixel value
        Eigen::Vector2d dist_pix;
        cam.GetParameters().Convert<camera::UNDISTORTED_C, camera::DISTORTED>(undist_centered_pix, &dist_pix);

        // Skip pixels that don't project in the image, and potentially nan pixels.
        bool is_good = (dist_pix.x() >= 0 && dist_pix.x() < calib_image_cols - 1 && dist_pix.y() >= 0 &&
                        dist_pix.y() < calib_image_rows - 1);
        if (!is_good) continue;

        // Find the pixel value using bilinear interpolation. Note
        // that we compensate for the image being larger by 'factor'
        // compared to what is calibrated.
        // TODO(oalexan1): Maybe use bicubic
        cv::Size s(1, 1);
        cv::Mat interp_pix_val;
        cv::Point2f pix;
        pix.x = factor * dist_pix[0];
        pix.y = factor * dist_pix[1];
        cv::getRectSubPix(image, s, pix, interp_pix_val);
        cv::Vec3b color = interp_pix_val.at<cv::Vec3b>(0, 0);

        // Find the location where to put the pixel
        int offset = texture->channels() * ((F.shift_u + iy) + (F.shift_v + iz) * texture->width());

        // Copy the color
        for (int channel = 0; channel < NUM_CHANNELS - 1; channel++) texture_ptr[offset + channel] = color[channel];

        // Make it non-transparent
        texture_ptr[offset + NUM_CHANNELS - 1] = 255;
      }
    }

#pragma omp critical
    {
      // Here we integrate the results from each loop iteration so have
      // to use a lock (critical section).

      // Sanity check, to ensure nothing got mixed up with the multiple
      // threads
      if (cost_val >= smallest_cost_per_face[face_id]) LOG(FATAL) << "Book-keeping error in estimating cost per face.";

      smallest_cost_per_face[face_id] = cost_val;
    }
  }  // End loop over mesh faces

  // Create an OpenCV matrix in place, for export. Note that we have four channels
  out_texture = cv::Mat(texture->height(), texture->width(), CV_8UC4, texture_ptr);

  // std::cout << "Projecting took: " << timer.get_elapsed()/1000.0 << "
  // seconds\n";
}
}  // namespace dense_map
