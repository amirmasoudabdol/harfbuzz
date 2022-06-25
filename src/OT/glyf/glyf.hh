#ifndef OT_GLYF_GLYF_HH
#define OT_GLYF_GLYF_HH


#include "hb-open-type.hh"
#include "hb-ot-head-table.hh"
#include "hb-ot-hmtx-table.hh"
#include "hb-ot-var-gvar-table.hh"
#include "hb-draw.hh"

#include "OT/glyf/Glyph.hh"
#include "OT/glyf/loca.hh"


namespace OT {


/*
 * glyf -- TrueType Glyph Data
 * https://docs.microsoft.com/en-us/typography/opentype/spec/glyf
 */
#define HB_OT_TAG_glyf HB_TAG('g','l','y','f')


struct glyf
{
  friend struct glyf_accelerator_t;

  static constexpr hb_tag_t tableTag = HB_OT_TAG_glyf;

  bool sanitize (hb_sanitize_context_t *c HB_UNUSED) const
  {
    TRACE_SANITIZE (this);
    /* Runtime checks as eager sanitizing each glyph is costy */
    return_trace (true);
  }

  template<typename Iterator,
	   hb_requires (hb_is_source_of (Iterator, unsigned int))>
  static bool
  _add_loca_and_head (hb_subset_plan_t * plan, Iterator padded_offsets, bool use_short_loca)
  {
    unsigned num_offsets = padded_offsets.len () + 1;
    unsigned entry_size = use_short_loca ? 2 : 4;
    char *loca_prime_data = (char *) hb_calloc (entry_size, num_offsets);

    if (unlikely (!loca_prime_data)) return false;

    DEBUG_MSG (SUBSET, nullptr, "loca entry_size %d num_offsets %d size %d",
	       entry_size, num_offsets, entry_size * num_offsets);

    if (use_short_loca)
      _write_loca (padded_offsets, true, hb_array ((HBUINT16 *) loca_prime_data, num_offsets));
    else
      _write_loca (padded_offsets, false, hb_array ((HBUINT32 *) loca_prime_data, num_offsets));

    hb_blob_t *loca_blob = hb_blob_create (loca_prime_data,
					   entry_size * num_offsets,
					   HB_MEMORY_MODE_WRITABLE,
					   loca_prime_data,
					   hb_free);

    bool result = plan->add_table (HB_OT_TAG_loca, loca_blob)
	       && _add_head_and_set_loca_version (plan, use_short_loca);

    hb_blob_destroy (loca_blob);
    return result;
  }

  template<typename IteratorIn, typename IteratorOut,
	   hb_requires (hb_is_source_of (IteratorIn, unsigned int)),
	   hb_requires (hb_is_sink_of (IteratorOut, unsigned))>
  static void
  _write_loca (IteratorIn it, bool short_offsets, IteratorOut dest)
  {
    unsigned right_shift = short_offsets ? 1 : 0;
    unsigned int offset = 0;
    dest << 0;
    + it
    | hb_map ([=, &offset] (unsigned int padded_size)
	      {
		offset += padded_size;
		DEBUG_MSG (SUBSET, nullptr, "loca entry offset %d", offset);
		return offset >> right_shift;
	      })
    | hb_sink (dest)
    ;
  }

  /* requires source of SubsetGlyph complains the identifier isn't declared */
  template <typename Iterator>
  bool serialize (hb_serialize_context_t *c,
		  Iterator it,
                  bool use_short_loca,
		  const hb_subset_plan_t *plan)
  {
    TRACE_SERIALIZE (this);
    unsigned init_len = c->length ();
    for (const auto &_ : it) _.serialize (c, use_short_loca, plan);

    /* As a special case when all glyph in the font are empty, add a zero byte
     * to the table, so that OTS doesn’t reject it, and to make the table work
     * on Windows as well.
     * See https://github.com/khaledhosny/ots/issues/52 */
    if (init_len == c->length ())
    {
      HBUINT8 empty_byte;
      empty_byte = 0;
      c->copy (empty_byte);
    }
    return_trace (true);
  }

  /* Byte region(s) per glyph to output
     unpadded, hints removed if so requested
     If we fail to process a glyph we produce an empty (0-length) glyph */
  bool subset (hb_subset_context_t *c) const
  {
    TRACE_SUBSET (this);

    glyf *glyf_prime = c->serializer->start_embed <glyf> ();
    if (unlikely (!c->serializer->check_success (glyf_prime))) return_trace (false);

    hb_vector_t<SubsetGlyph> glyphs;
    _populate_subset_glyphs (c->plan, &glyphs);

    auto padded_offsets =
    + hb_iter (glyphs)
    | hb_map (&SubsetGlyph::padded_size)
    ;

    unsigned max_offset = + padded_offsets | hb_reduce (hb_add, 0);
    bool use_short_loca = max_offset < 0x1FFFF;


    glyf_prime->serialize (c->serializer, hb_iter (glyphs), use_short_loca, c->plan);
    if (!use_short_loca) {
      padded_offsets =
          + hb_iter (glyphs)
          | hb_map (&SubsetGlyph::length)
          ;
    }


    if (unlikely (c->serializer->in_error ())) return_trace (false);
    return_trace (c->serializer->check_success (_add_loca_and_head (c->plan,
								    padded_offsets,
                                                                    use_short_loca)));
  }

  static bool
  _add_head_and_set_loca_version (hb_subset_plan_t *plan, bool use_short_loca)
  {
    hb_blob_t *head_blob = hb_sanitize_context_t ().reference_table<head> (plan->source);
    hb_blob_t *head_prime_blob = hb_blob_copy_writable_or_fail (head_blob);
    hb_blob_destroy (head_blob);

    if (unlikely (!head_prime_blob))
      return false;

    head *head_prime = (head *) hb_blob_get_data_writable (head_prime_blob, nullptr);
    head_prime->indexToLocFormat = use_short_loca ? 0 : 1;
    bool success = plan->add_table (HB_OT_TAG_head, head_prime_blob);

    hb_blob_destroy (head_prime_blob);
    return success;
  }

  struct SubsetGlyph
  {
    hb_codepoint_t new_gid;
    hb_codepoint_t old_gid;
    Glyph source_glyph;
    hb_bytes_t dest_start;  /* region of source_glyph to copy first */
    hb_bytes_t dest_end;    /* region of source_glyph to copy second */

    bool serialize (hb_serialize_context_t *c,
                    bool use_short_loca,
		    const hb_subset_plan_t *plan) const
    {
      TRACE_SERIALIZE (this);

      hb_bytes_t dest_glyph = dest_start.copy (c);
      dest_glyph = hb_bytes_t (&dest_glyph, dest_glyph.length + dest_end.copy (c).length);
      unsigned int pad_length = use_short_loca ? padding () : 0;
      DEBUG_MSG (SUBSET, nullptr, "serialize %d byte glyph, width %d pad %d", dest_glyph.length, dest_glyph.length + pad_length, pad_length);

      HBUINT8 pad;
      pad = 0;
      while (pad_length > 0)
      {
	c->embed (pad);
	pad_length--;
      }

      if (unlikely (!dest_glyph.length)) return_trace (true);

      /* update components gids */
      for (auto &_ : Glyph (dest_glyph).get_composite_iterator ())
      {
	hb_codepoint_t new_gid;
	if (plan->new_gid_for_old_gid (_.get_glyph_index (), &new_gid))
	  const_cast<CompositeGlyphChain &> (_).set_glyph_index (new_gid);
      }

      if (plan->flags & HB_SUBSET_FLAGS_NO_HINTING)
        Glyph (dest_glyph).drop_hints ();

      if (plan->flags & HB_SUBSET_FLAGS_SET_OVERLAPS_FLAG)
        Glyph (dest_glyph).set_overlaps_flag ();

      return_trace (true);
    }

    void drop_hints_bytes ()
    { source_glyph.drop_hints_bytes (dest_start, dest_end); }

    unsigned int      length () const { return dest_start.length + dest_end.length; }
    /* pad to 2 to ensure 2-byte loca will be ok */
    unsigned int     padding () const { return length () % 2; }
    unsigned int padded_size () const { return length () + padding (); }
  };

  void
  _populate_subset_glyphs (const hb_subset_plan_t   *plan,
			   hb_vector_t<SubsetGlyph> *glyphs /* OUT */) const;

  protected:
  UnsizedArrayOf<HBUINT8>
		dataZ;	/* Glyphs data. */
  public:
  DEFINE_SIZE_MIN (0);	/* In reality, this is UNBOUNDED() type; but since we always
			 * check the size externally, allow Null() object of it by
			 * defining it _MIN instead. */
};

struct glyf_accelerator_t
{
  glyf_accelerator_t (hb_face_t *face)
  {
    short_offset = false;
    num_glyphs = 0;
    loca_table = nullptr;
    glyf_table = nullptr;
#ifndef HB_NO_VAR
    gvar = nullptr;
#endif
    hmtx = nullptr;
#ifndef HB_NO_VERTICAL
    vmtx = nullptr;
#endif
    const OT::head &head = *face->table.head;
    if (head.indexToLocFormat > 1 || head.glyphDataFormat > 0)
      /* Unknown format.  Leave num_glyphs=0, that takes care of disabling us. */
      return;
    short_offset = 0 == head.indexToLocFormat;

    loca_table = face->table.loca.get_blob (); // Needs no destruct!
    glyf_table = hb_sanitize_context_t ().reference_table<glyf> (face);
#ifndef HB_NO_VAR
    gvar = face->table.gvar;
#endif
    hmtx = face->table.hmtx;
#ifndef HB_NO_VERTICAL
    vmtx = face->table.vmtx;
#endif

    num_glyphs = hb_max (1u, loca_table.get_length () / (short_offset ? 2 : 4)) - 1;
    num_glyphs = hb_min (num_glyphs, face->get_num_glyphs ());
  }
  ~glyf_accelerator_t ()
  {
    glyf_table.destroy ();
  }

  bool has_data () const { return num_glyphs; }

  protected:
  template<typename T>
  bool get_points (hb_font_t *font, hb_codepoint_t gid, T consumer) const
  {
    if (gid >= num_glyphs) return false;

    /* Making this allocfree is not that easy
       https://github.com/harfbuzz/harfbuzz/issues/2095
       mostly because of gvar handling in VF fonts,
       perhaps a separate path for non-VF fonts can be considered */
    contour_point_vector_t all_points;

    bool phantom_only = !consumer.is_consuming_contour_points ();
    if (unlikely (!glyph_for_gid (gid).get_points (font, *this, all_points, phantom_only)))
      return false;

    if (consumer.is_consuming_contour_points ())
    {
      for (unsigned point_index = 0; point_index + 4 < all_points.length; ++point_index)
	consumer.consume_point (all_points[point_index]);
      consumer.points_end ();
    }

    /* Where to write phantoms, nullptr if not requested */
    contour_point_t *phantoms = consumer.get_phantoms_sink ();
    if (phantoms)
      for (unsigned i = 0; i < PHANTOM_COUNT; ++i)
	phantoms[i] = all_points[all_points.length - PHANTOM_COUNT + i];

    return true;
  }

#ifndef HB_NO_VAR
  struct points_aggregator_t
  {
    hb_font_t *font;
    hb_glyph_extents_t *extents;
    contour_point_t *phantoms;

    struct contour_bounds_t
    {
      contour_bounds_t () { min_x = min_y = FLT_MAX; max_x = max_y = -FLT_MAX; }

      void add (const contour_point_t &p)
      {
	min_x = hb_min (min_x, p.x);
	min_y = hb_min (min_y, p.y);
	max_x = hb_max (max_x, p.x);
	max_y = hb_max (max_y, p.y);
      }

      bool empty () const { return (min_x >= max_x) || (min_y >= max_y); }

      void get_extents (hb_font_t *font, hb_glyph_extents_t *extents)
      {
	if (unlikely (empty ()))
	{
	  extents->width = 0;
	  extents->x_bearing = 0;
	  extents->height = 0;
	  extents->y_bearing = 0;
	  return;
	}
	extents->x_bearing = font->em_scalef_x (min_x);
	extents->width = font->em_scalef_x (max_x) - extents->x_bearing;
	extents->y_bearing = font->em_scalef_y (max_y);
	extents->height = font->em_scalef_y (min_y) - extents->y_bearing;
      }

      protected:
      float min_x, min_y, max_x, max_y;
    } bounds;

    points_aggregator_t (hb_font_t *font_, hb_glyph_extents_t *extents_, contour_point_t *phantoms_)
    {
      font = font_;
      extents = extents_;
      phantoms = phantoms_;
      if (extents) bounds = contour_bounds_t ();
    }

    void consume_point (const contour_point_t &point) { bounds.add (point); }
    void points_end () { bounds.get_extents (font, extents); }

    bool is_consuming_contour_points () { return extents; }
    contour_point_t *get_phantoms_sink () { return phantoms; }
  };

  public:
  unsigned
  get_advance_var (hb_font_t *font, hb_codepoint_t gid, bool is_vertical) const
  {
    if (unlikely (gid >= num_glyphs)) return 0;

    bool success = false;

    contour_point_t phantoms[PHANTOM_COUNT];
    if (likely (font->num_coords == gvar->get_axis_count ()))
      success = get_points (font, gid, points_aggregator_t (font, nullptr, phantoms));

    if (unlikely (!success))
      return
#ifndef HB_NO_VERTICAL
	is_vertical ? vmtx->get_advance (gid) :
#endif
	hmtx->get_advance (gid);

    float result = is_vertical
		 ? phantoms[PHANTOM_TOP].y - phantoms[PHANTOM_BOTTOM].y
		 : phantoms[PHANTOM_RIGHT].x - phantoms[PHANTOM_LEFT].x;
    return hb_clamp (roundf (result), 0.f, (float) UINT_MAX / 2);
  }

  int get_side_bearing_var (hb_font_t *font, hb_codepoint_t gid, bool is_vertical) const
  {
    if (unlikely (gid >= num_glyphs)) return 0;

    hb_glyph_extents_t extents;

    contour_point_t phantoms[PHANTOM_COUNT];
    if (unlikely (!get_points (font, gid, points_aggregator_t (font, &extents, phantoms))))
      return
#ifndef HB_NO_VERTICAL
	is_vertical ? vmtx->get_side_bearing (gid) :
#endif
	hmtx->get_side_bearing (gid);

    return is_vertical
	 ? ceilf (phantoms[PHANTOM_TOP].y) - extents.y_bearing
	 : floorf (phantoms[PHANTOM_LEFT].x);
  }
#endif

  public:
  bool get_extents (hb_font_t *font, hb_codepoint_t gid, hb_glyph_extents_t *extents) const
  {
    if (unlikely (gid >= num_glyphs)) return false;

#ifndef HB_NO_VAR
    if (font->num_coords && font->num_coords == gvar->get_axis_count ())
      return get_points (font, gid, points_aggregator_t (font, extents, nullptr));
#endif
    return glyph_for_gid (gid).get_extents (font, *this, extents);
  }

  const Glyph
  glyph_for_gid (hb_codepoint_t gid, bool needs_padding_removal = false) const
  {
    if (unlikely (gid >= num_glyphs)) return Glyph ();

    unsigned int start_offset, end_offset;

    if (short_offset)
    {
      const HBUINT16 *offsets = (const HBUINT16 *) loca_table->dataZ.arrayZ;
      start_offset = 2 * offsets[gid];
      end_offset   = 2 * offsets[gid + 1];
    }
    else
    {
      const HBUINT32 *offsets = (const HBUINT32 *) loca_table->dataZ.arrayZ;
      start_offset = offsets[gid];
      end_offset   = offsets[gid + 1];
    }

    if (unlikely (start_offset > end_offset || end_offset > glyf_table.get_length ()))
      return Glyph ();

    Glyph glyph (hb_bytes_t ((const char *) this->glyf_table + start_offset,
			     end_offset - start_offset), gid);
    return needs_padding_removal ? glyph.trim_padding () : glyph;
  }

  struct path_builder_t
  {
    hb_font_t *font;
    hb_draw_session_t *draw_session;

    struct optional_point_t
    {
      optional_point_t () { has_data = false; }
      optional_point_t (float x_, float y_) { x = x_; y = y_; has_data = true; }

      bool has_data;
      float x;
      float y;

      optional_point_t lerp (optional_point_t p, float t)
      { return optional_point_t (x + t * (p.x - x), y + t * (p.y - y)); }
    } first_oncurve, first_offcurve, last_offcurve;

    path_builder_t (hb_font_t *font_, hb_draw_session_t &draw_session_)
    {
      font = font_;
      draw_session = &draw_session_;
      first_oncurve = first_offcurve = last_offcurve = optional_point_t ();
    }

    /* based on https://github.com/RazrFalcon/ttf-parser/blob/4f32821/src/glyf.rs#L287
       See also:
       * https://developer.apple.com/fonts/TrueType-Reference-Manual/RM01/Chap1.html
       * https://stackoverflow.com/a/20772557 */
    void consume_point (const contour_point_t &point)
    {
      bool is_on_curve = point.flag & Glyph::FLAG_ON_CURVE;
      optional_point_t p (point.x, point.y);
      if (!first_oncurve.has_data)
      {
	if (is_on_curve)
	{
	  first_oncurve = p;
	  draw_session->move_to (font->em_fscalef_x (p.x), font->em_fscalef_y (p.y));
	}
	else
	{
	  if (first_offcurve.has_data)
	  {
	    optional_point_t mid = first_offcurve.lerp (p, .5f);
	    first_oncurve = mid;
	    last_offcurve = p;
	    draw_session->move_to (font->em_fscalef_x (mid.x), font->em_fscalef_y (mid.y));
	  }
	  else
	    first_offcurve = p;
	}
      }
      else
      {
	if (last_offcurve.has_data)
	{
	  if (is_on_curve)
	  {
	    draw_session->quadratic_to (font->em_fscalef_x (last_offcurve.x), font->em_fscalef_y (last_offcurve.y),
				       font->em_fscalef_x (p.x), font->em_fscalef_y (p.y));
	    last_offcurve = optional_point_t ();
	  }
	  else
	  {
	    optional_point_t mid = last_offcurve.lerp (p, .5f);
	    draw_session->quadratic_to (font->em_fscalef_x (last_offcurve.x), font->em_fscalef_y (last_offcurve.y),
				       font->em_fscalef_x (mid.x), font->em_fscalef_y (mid.y));
	    last_offcurve = p;
	  }
	}
	else
	{
	  if (is_on_curve)
	    draw_session->line_to (font->em_fscalef_x (p.x), font->em_fscalef_y (p.y));
	  else
	    last_offcurve = p;
	}
      }

      if (point.is_end_point)
      {
	if (first_offcurve.has_data && last_offcurve.has_data)
	{
	  optional_point_t mid = last_offcurve.lerp (first_offcurve, .5f);
	  draw_session->quadratic_to (font->em_fscalef_x (last_offcurve.x), font->em_fscalef_y (last_offcurve.y),
				     font->em_fscalef_x (mid.x), font->em_fscalef_y (mid.y));
	  last_offcurve = optional_point_t ();
	  /* now check the rest */
	}

	if (first_offcurve.has_data && first_oncurve.has_data)
	  draw_session->quadratic_to (font->em_fscalef_x (first_offcurve.x), font->em_fscalef_y (first_offcurve.y),
				     font->em_fscalef_x (first_oncurve.x), font->em_fscalef_y (first_oncurve.y));
	else if (last_offcurve.has_data && first_oncurve.has_data)
	  draw_session->quadratic_to (font->em_fscalef_x (last_offcurve.x), font->em_fscalef_y (last_offcurve.y),
				     font->em_fscalef_x (first_oncurve.x), font->em_fscalef_y (first_oncurve.y));
	else if (first_oncurve.has_data)
	  draw_session->line_to (font->em_fscalef_x (first_oncurve.x), font->em_fscalef_y (first_oncurve.y));
	else if (first_offcurve.has_data)
	{
	  float x = font->em_fscalef_x (first_offcurve.x), y = font->em_fscalef_x (first_offcurve.y);
	  draw_session->move_to (x, y);
	  draw_session->quadratic_to (x, y, x, y);
	}

	/* Getting ready for the next contour */
	first_oncurve = first_offcurve = last_offcurve = optional_point_t ();
	draw_session->close_path ();
      }
    }
    void points_end () {}

    bool is_consuming_contour_points () { return true; }
    contour_point_t *get_phantoms_sink () { return nullptr; }
  };

  bool
  get_path (hb_font_t *font, hb_codepoint_t gid, hb_draw_session_t &draw_session) const
  { return get_points (font, gid, path_builder_t (font, draw_session)); }

#ifndef HB_NO_VAR
  const gvar_accelerator_t *gvar;
#endif
  const hmtx_accelerator_t *hmtx;
#ifndef HB_NO_VERTICAL
  const vmtx_accelerator_t *vmtx;
#endif

  private:
  bool short_offset;
  unsigned int num_glyphs;
  hb_blob_ptr_t<loca> loca_table;
  hb_blob_ptr_t<glyf> glyf_table;
};


inline void
glyf::_populate_subset_glyphs (const hb_subset_plan_t   *plan,
			       hb_vector_t<SubsetGlyph> *glyphs /* OUT */) const
{
  OT::glyf_accelerator_t glyf (plan->source);

  + hb_range (plan->num_output_glyphs ())
  | hb_map ([&] (hb_codepoint_t new_gid)
	{
	  SubsetGlyph subset_glyph = {0};
	  subset_glyph.new_gid = new_gid;

	  /* should never fail: all old gids should be mapped */
	  if (!plan->old_gid_for_new_gid (new_gid, &subset_glyph.old_gid))
	    return subset_glyph;

	  if (new_gid == 0 &&
	      !(plan->flags & HB_SUBSET_FLAGS_NOTDEF_OUTLINE))
	    subset_glyph.source_glyph = Glyph ();
	  else
	    subset_glyph.source_glyph = glyf.glyph_for_gid (subset_glyph.old_gid, true);
	  if (plan->flags & HB_SUBSET_FLAGS_NO_HINTING)
	    subset_glyph.drop_hints_bytes ();
	  else
	    subset_glyph.dest_start = subset_glyph.source_glyph.get_bytes ();
	  return subset_glyph;
	})
  | hb_sink (glyphs)
  ;
}



} /* namespace OT */


#endif /* OT_GLYF_GLYF_HH */
