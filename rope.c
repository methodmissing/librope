//
//  rope.c
//  librope
//
//  Created by Joseph Gentle on 20/08/12.
//  Copyright (c) 2012 Joseph Gentle. All rights reserved.
//

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "rope.h"

typedef struct rope_node_t {
  uint8_t str[ROPE_NODE_STR_SIZE];

  // The number of bytes in str in use
  uint16_t num_bytes;
  
  // This is the number of elements allocated in nexts.
  // Each height is 1/2 as likely as the height before. The minimum height is 1.
  uint8_t height;
  
  // unused for now - should be useful for object pools.
  //uint8_t height_capacity;
  
  rope_next_node nexts[0];
} rope_node;

// Create a new rope with no contents
rope *rope_new() {
  rope *r = calloc(1, sizeof(rope));
  r->height = 0;
  r->height_capacity = 10;
  r->heads = malloc(sizeof(rope_next_node) * 10);
  return r;
}

// Create a new rope with no contents
rope *rope_new_with_utf8(const uint8_t *str) {
  rope *r = rope_new();
  rope_insert(r, 0, str);
  return r;
}

// Free the specified rope
void rope_free(rope *r) {
  assert(r);
  rope_node *next;
  for (rope_node *n = r->heads[0].node; n != NULL; n = next) {
    next = n->nexts[0].node;
    free(n);
  }
  
  free(r->heads);
  free(r);
}

// Create a new C string which contains the rope. The string will contain
// the rope encoded as utf-8.
uint8_t *rope_createcstr(rope *r, size_t *len) {
  size_t numbytes = rope_byte_count(r);
  uint8_t *bytes = malloc(numbytes + 1); // Room for a zero.
  bytes[numbytes] = '\0';
  
  uint8_t *p = bytes;
  for (rope_node *n = r->heads[0].node; n != NULL; n = n->nexts[0].node) {
    memcpy(p, n->str, n->num_bytes);
    p += n->num_bytes;
  }
  
  assert(p == &bytes[numbytes]);
  
  if (len) {
    *len = numbytes;
  }
  
  return bytes;
}

// Get the number of characters in a rope
size_t rope_char_count(rope *r) {
  assert(r);
  return r->num_chars;
}

// Get the number of bytes which the rope would take up if stored as a utf8
// string
size_t rope_byte_count(rope *r) {
  assert(r);
  return r->num_bytes;
}

#define MIN(x,y) ((x) > (y) ? (y) : (x))
#define MAX(x,y) ((x) > (y) ? (x) : (y))

static uint8_t random_height() {
  uint8_t height = 1;
  
  while(height <= UINT8_MAX && random() % 1) {
    height++;
  }
  
  return height;
}

// Figure out how many bytes to allocate for a node with the specified height.
static size_t node_size(uint8_t height) {
  return sizeof(rope_node) + height * sizeof(rope_next_node);
}

// Allocate and return a new node. The new node will be full of junk, except
// for its height.
// This function should be replaced at some point with an object pool based version.
static rope_node *alloc_node(uint8_t height) {
  rope_node *node = malloc(node_size(height));
  node->height = height;
  return node;
}

static size_t codepoint_size(uint8_t byte) {
  if (byte <= 0x7f) { return 1; }
  else if (byte <= 0xdf) { return 2; }
  else if (byte <= 0xef) { return 3; }
  else if (byte <= 0xf7) { return 4; }
  else if (byte <= 0xfb) { return 5; }
  else if (byte <= 0xfd) { return 6; }
  else {
    // The codepoint is invalid... what do?
    assert(0);
    return 1;
  }
}

// Internal method of rope_insert.
static void insert_at(rope *r, size_t pos, const uint8_t *str,
    size_t num_bytes, size_t num_chars, rope_node *nodes[], size_t tree_offsets[]) {
  // This describes how many of the nodes[] and tree_offsets[] arrays are filled in.
  uint8_t max_height = r->height;
  uint8_t new_height = random_height();
  rope_node *new_node = alloc_node(new_height);
  new_node->num_bytes = num_bytes;
  memcpy(new_node->str, str, num_bytes);
  
  // Ensure the rope has enough capacity to store the next pointers to the new object.
  if (new_height > max_height) {
    r->height = new_height;
    if (r->height > r->height_capacity) {
      do {
        r->height_capacity *= 2;
      } while (r->height_capacity < r->height);
      r->heads = realloc(r->heads, sizeof(rope_next_node) * r->height_capacity);
    }
  }

  // Fill in the new node's nexts array.
  int i;
  for (i = 0; i < new_height; i++) {
    if (i < max_height) {
      rope_next_node *prev_node = (nodes[i] ? &nodes[i]->nexts[i] : &r->heads[i]);
      new_node->nexts[i].node = prev_node->node;
      new_node->nexts[i].skip_size = num_chars + prev_node->skip_size - tree_offsets[i];

      prev_node->node = new_node;
      prev_node->skip_size = tree_offsets[i];
    } else {
      // Edit the head node instead of editing the parent listed in nodes.
      new_node->nexts[i].node = NULL;
      new_node->nexts[i].skip_size = r->num_chars - pos + num_chars;
      
      r->heads[i].node = new_node;
      r->heads[i].skip_size = pos;
    }
    
    nodes[i] = new_node;
    tree_offsets[i] = num_chars;
  }
  
  for (; i < max_height; i++) {
    if (nodes[i]) {
      nodes[i]->nexts[i].skip_size += num_chars;
    } else {
      r->heads[i].skip_size += num_chars;
    }
    tree_offsets[i] += num_chars;
  }
  
  r->num_chars += num_chars;
  r->num_bytes += num_bytes;
}

// Insert the given utf8 string into the rope at the specified position.
void rope_insert(rope *r, size_t pos, const uint8_t *str) {
  assert(r);
  assert(str);
  pos = MIN(pos, r->num_chars);
  
  // First we need to search for the node where we'll insert the string.
  rope_node *e = NULL;

  uint8_t height = r->height;
  // There's a good chance we'll have to rewrite a bunch of next pointers and a bunch
  // of offsets. This variable will store pointers to the elements which need to
  // be changed.
  rope_node *nodes[UINT8_MAX];
  size_t tree_offsets[UINT8_MAX];
  
  // Offset stores how characters we still need to skip in the current node.
  size_t offset = pos;
  while (height--) {
    size_t skip;
    while (offset > (skip = (e ? e->nexts : r->heads)[height].skip_size)) {
      // Go right.
      offset -= skip;
      e = (e ? e->nexts : r->heads)[height].node;
    }
    
    // Go down.
    // Record the distance from the start of the current node to the inserted text.
    tree_offsets[height] = offset;
    nodes[height] = e;
  }
  
  // offset contains how far (in characters) into the current element to skip.
  // Figure out how much that is in bytes.
  size_t offset_bytes = 0;
  if (e && offset) {
    uint8_t *p = e->str;
    
    for (int i = 0; i < offset; i++) {
      p += codepoint_size(*p);
    }
    offset_bytes = p - e->str;
  }
  
  // Maybe we can insert the characters into the current node?
  size_t num_inserted_bytes = strlen((char *)str);
  if (e && e->num_bytes + num_inserted_bytes <= ROPE_NODE_STR_SIZE) {
    // First move the current bytes later on in the string.
    if (offset_bytes < e->num_bytes) {
      memmove(&e->str[offset_bytes + num_inserted_bytes],
              &e->str[offset_bytes],
              e->num_bytes - offset_bytes);
    }
    
    // Then copy in the string bytes
    memcpy(&e->str[offset_bytes], str, num_inserted_bytes);
  } else {
    // There isn't room. We'll need to add at least one new node to the rope.
    
    // If we're not at the end of the current node, we'll need to remove
    // the end of the current node's data and reinsert it later.
    size_t num_end_bytes = 0, num_end_chars;
    if (e) {
      num_end_bytes = e->num_bytes - offset_bytes;
      e->num_bytes = offset_bytes;
      if (num_end_bytes) {
        // Count out characters.
        num_end_chars = e->nexts[0].skip_size - offset;
        for (int i = 0; i < r->height; i++) {
          nodes[i]->nexts[i].skip_size -= num_end_bytes;
        }
        
        r->num_chars -= num_end_chars;
        r->num_bytes -= num_end_bytes;
      }
    }
    
    // Now, we insert new node[s] containing the data. The data must
    // be broken into pieces of with a maximum size of ROPE_NODE_STR_SIZE.
    // Node boundaries do not occur in the middle of a utf8 codepoint.
    size_t str_offset = 0;
    while (str_offset < num_inserted_bytes) {
      size_t new_node_bytes = 0;
      size_t new_node_chars = 0;
      
      while (str_offset + new_node_bytes < num_inserted_bytes) {
        size_t cs = codepoint_size(str[str_offset + new_node_bytes]);
        if (cs + new_node_bytes > ROPE_NODE_STR_SIZE) {
          break;
        } else {
          new_node_bytes += cs;
          new_node_chars++;
        }
      }
      
      insert_at(r, pos, &str[str_offset], new_node_bytes, new_node_chars, nodes, tree_offsets);
      pos += new_node_chars;
      str_offset += new_node_bytes;
    }
    
    if (num_end_bytes) {
      insert_at(r, pos, &e->str[offset_bytes], num_end_bytes, num_end_chars, nodes, tree_offsets);
    }
  }
}

// Delete num characters at position pos. Deleting past the end of the string
// has no effect.
void rope_del(rope *r, size_t pos, size_t num) {
  
}

