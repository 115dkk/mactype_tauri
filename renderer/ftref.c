#include "ftref.h"

FT_Error FT_Glyph_Ref_Copy( FT_Referenced_Glyph source,  FT_Referenced_Glyph *target )
{
	if (target == NULL)
		return 1;
	*target = NULL;
	if (source == NULL || source->ft_glyph == NULL)
		return 1;
	if (source->refcount<0)
		return 1;
	if (source->ft_glyph->format == FT_GLYPH_FORMAT_NONE)
		return 2;
	InterlockedIncrement(&source->refcount);
	*target = source;
	return 0;
}

void FT_Done_Ref_Glyph( FT_Referenced_Glyph *glyph )
{
	FT_Referenced_Glyph current;
	if (glyph == NULL || *glyph == NULL)
		return;
	current = *glyph;
	*glyph = NULL;
	if (InterlockedDecrement(&current->refcount) == 0)
	{
		if (current->ft_glyph && current->ft_glyph->library)
			FT_Done_Glyph(current->ft_glyph);
		free(current);
	}
}

FT_Referenced_Glyph New_FT_Ref_Glyph()
{
	FT_Referenced_Glyph copy = (FT_Referenced_Glyph)calloc(1, sizeof(FT_Referenced_GlyphRec));
	if (copy != NULL)
		copy->refcount = 1;
	return copy;
}
