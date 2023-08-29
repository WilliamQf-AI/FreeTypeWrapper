﻿#include <locale>
#include <vector>
#include <string>
#include <iostream>

#include <FreeTypeWrapper/FreeTypeConnector.h>
#include <FreeTypeRenderer.h>
#include <FreeTypeFont.h>

#include <LLUtils/Exception.h>
#include <LLUtils/StringUtility.h>
#include <LLUtils/Utility.h>
#include <LLUtils/Color.h>
#include <LLUtils/Buffer.h>

#include "BlitBox.h"
#include "MetaTextParser.h"
#if FREETYPE_WRAPPER_BUILD_FRIBIDI == 1
    #include <fribidi.h>
#endif

#include <ww898/utf_converters.hpp>


namespace FreeType
{

    FreeTypeConnector::FreeTypeConnector()
    {

        if (FT_Error error = FT_Init_FreeType(&fLibrary); error != FT_Err_Ok)
            LL_EXCEPTION(LLUtils::Exception::ErrorCode::RuntimeError, GenerateFreeTypeErrorString("can not load glyph", error));

    }

    FreeTypeConnector::~FreeTypeConnector()
    {
        fFontNameToFont.clear();
        FT_Stroker_Done(fStroker);
        FT_Error error = FT_Done_FreeType(fLibrary);
        if (error)
        {
            //TODO: destory freetype library eariler before object destruction.
            //throw std::logic_error("Can no destroy freetype library");
        }
    }


    std::string FreeTypeConnector::GenerateFreeTypeErrorString(std::string userMessage, FT_Error  error)
    {
        using namespace std::string_literals;
        return "FreeType error: "s + FT_Error_String(error) + ", " + userMessage;
    }


    template <typename string_type>
    std::u32string bidi_string(const string_type& logical)
    {
#if FREETYPE_WRAPPER_BUILD_FRIBIDI == 1

        FriBidiParType base = FRIBIDI_PAR_ON;
        FriBidiStrIndex* ltov, * vtol;
        FriBidiLevel* levels;
        fribidi_boolean log2vis;
        std::u32string logicalUTF32 = ww898::utf::convz<char32_t>(logical);
        std::u32string visualUTF32(logicalUTF32.length(), 0);

        ltov = nullptr;
        vtol = nullptr;
        levels = nullptr;

        log2vis = fribidi_log2vis(reinterpret_cast<FriBidiChar*>(logicalUTF32.data()),static_cast<FriBidiStrIndex>(logicalUTF32.length()), &base,
            /* output */
            reinterpret_cast<FriBidiChar*>(visualUTF32.data()), ltov, vtol, levels);
        
        if (!log2vis) 
            LL_EXCEPTION(LLUtils::Exception::ErrorCode::InvalidState, "Cannot process string");
        
        return visualUTF32;
#else
        return ww898::utf::convz<char32_t>(logical);
#endif
    }


    
    void FreeTypeConnector::MeasureText(const TextMesureParams& measureParams, TextMetrics& mesureResult)
    {
        using namespace std;
        using namespace LLUtils;

        // If enabled, outline is not rendererd, but an estimation is given for the size of the final image
        constexpr bool OptimizeOutlineRendering = true;

        auto textCreateParams = measureParams.createParams;

        const  std::wstring text = textCreateParams.text;
        const std::wstring& fontPath = textCreateParams.fontPath;
        const uint16_t fontSize = textCreateParams.fontSize;
        const uint32_t OutlineWidth = textCreateParams.outlineWidth;
        const LLUtils::Color outlineColor = textCreateParams.outlineColor;
        const LLUtils::Color backgroundColor = textCreateParams.backgroundColor;
        const FT_Render_Mode textRenderMOde = GetRenderMode(textCreateParams.renderMode);

        FreeTypeFont* font = GetOrCreateFont(fontPath);

        font->SetSize(fontSize, textCreateParams.DPIx, textCreateParams.DPIy);
        
        const bool renderOutline = OutlineWidth > 0;
        const bool renderText = true;

        mesureResult = {};

        int penX = 0;
        int penY = 0;
        mesureResult.lineMetrics.push_back({});
        LineMetrics* currentLine = &mesureResult.lineMetrics.back();
        

        FT_Face face = font->GetFace();
        const auto descender = face->size->metrics.descender >> 6;
        const uint32_t rowHeight = (static_cast<uint32_t>(face->size->metrics.height) >> 6) + textCreateParams.outlineWidth * 2;

        vector<FormattedTextEntry> formattedText;
        if ((textCreateParams.flags & TextCreateFlags::UseMetaText) == TextCreateFlags::UseMetaText)
            formattedText = MetaText::GetFormattedText(text);
        else
            formattedText.push_back({ textCreateParams.textColor, textCreateParams.text });

        for (const FormattedTextEntry& el : formattedText)
        {
            const std::u32string visualText = ((textCreateParams.flags & TextCreateFlags::Bidirectional) == TextCreateFlags::Bidirectional)
                ? bidi_string(el.text.c_str()) : ww898::utf::conv<char32_t>(el.text);

            for (const decltype(visualText)::value_type& codepoint : visualText)
            {
                if (codepoint == L'\n')
                {
                    penX = 0;
                    penY += rowHeight;
                    mesureResult.lineMetrics.push_back({});
                    currentLine = &mesureResult.lineMetrics.back();
                    continue;
                }

                const FT_UInt glyph_index = FT_Get_Char_Index(face, codepoint);
                if (FT_Error error = FT_Load_Glyph(
                    face,          /* handle to face object */
                    glyph_index,   /* glyph index           */
                    FT_LOAD_BITMAP_METRICS_ONLY); error != FT_Err_Ok)/* load flags, see below */
                {
                    LL_EXCEPTION(LLUtils::Exception::ErrorCode::RuntimeError, GenerateFreeTypeErrorString("can not Load glyph", error));
                }


                //measure outline
                const auto advance = face->glyph->advance.x >> 6;

                if (textCreateParams.maxWidthPx > 0 && penX + advance > static_cast<int>(textCreateParams.maxWidthPx))
                {
                    penY += rowHeight;
                    penX = 0;
                    mesureResult.lineMetrics.push_back({});
                    currentLine = &mesureResult.lineMetrics.back();
                }

                const auto baseVerticalPos = rowHeight + penY + descender;

                if (renderOutline && !OptimizeOutlineRendering) // measure outline
                {
                    //Generate outline bitmaps:
                    //TODO: optimize this out by using stroker metrics only if possible
                    //initialize stroker, so you can create outline font
                    FT_Stroker stroker = GetStroker();

                    //  2 * 64 result in 2px outline
                    FT_Stroker_Set(stroker, static_cast<FT_Fixed>(OutlineWidth * 64), FT_STROKER_LINECAP_ROUND, FT_STROKER_LINEJOIN_BEVEL, 0);
                    FT_Glyph glyph;
                    FT_Get_Glyph(face->glyph, &glyph);
                    FT_Glyph_StrokeBorder(&glyph, stroker, false, true);
                    FT_Glyph_To_Bitmap(&glyph, textRenderMOde, nullptr, true);

                    FT_BitmapGlyph bitmapGlyph = reinterpret_cast<FT_BitmapGlyph>(glyph);
                    FreeTypeRenderer::BitmapProperties bitmapProperties = FreeTypeRenderer::GetBitmapGlyphProperties(bitmapGlyph->bitmap);

                    auto width = bitmapProperties.width;
                    auto height = bitmapProperties.height;
                    auto left = bitmapGlyph->left;
                    auto top = bitmapGlyph->top;

                    currentLine->maxGlyphHeight = std::max<int32_t>(currentLine->maxGlyphHeight, height - top);
                    
                    mesureResult.minX = std::min<int32_t>(mesureResult.minX, left + penX);
                    mesureResult.maxX = std::max<int32_t>(mesureResult.maxX, left + width + penX );

                
                    FT_Done_Glyph(glyph);

                }
                // Render text
                if (renderText)
                {
                    FreeTypeRenderer::BitmapProperties bitmapProperties = FreeTypeRenderer::GetBitmapGlyphProperties(face->glyph->bitmap);

                    auto width = bitmapProperties.width;
                    auto height = bitmapProperties.height;
                    auto left = face->glyph->bitmap_left;
                    auto top = face->glyph->bitmap_top;
                    
                    if (OptimizeOutlineRendering == true && renderOutline == true)
                    {
                        //Do this for optimized estimation
                        currentLine->maxGlyphHeight = std::max<int32_t>(currentLine->maxGlyphHeight, height - top + OutlineWidth);
                        mesureResult.minX = std::min<int32_t>(mesureResult.minX, left + penX - OutlineWidth - 1);
                        mesureResult.maxX = std::max<int32_t>(mesureResult.maxX, left + width + penX + OutlineWidth + 1);
                    }
                    else
                    {
                        currentLine->maxGlyphHeight = std::max<int32_t>(currentLine->maxGlyphHeight, height - top);
                        mesureResult.minX = std::min<int32_t>(mesureResult.minX, left + penX);
                        mesureResult.maxX = std::max<int32_t>(mesureResult.maxX, left + width + penX);
                    }


                    penX += advance;
                }
            }
        }

        mesureResult.rect = { {mesureResult.minX , 0}, {mesureResult.maxX , static_cast<int32_t>(mesureResult.lineMetrics.size() * rowHeight) } };

        const auto baseVerticalPos = static_cast<int32_t>(mesureResult.lineMetrics.size() * rowHeight + descender );

        mesureResult.rect.RightBottom().y = std::max(mesureResult.rect.RightBottom().y, baseVerticalPos + mesureResult.lineMetrics.back().maxGlyphHeight 
            - static_cast<int32_t>(OutlineWidth));


        mesureResult.rect = mesureResult.rect.Infalte(measureParams.createParams.padding * 2, measureParams.createParams.padding * 2);
        mesureResult.rowHeight = static_cast<uint32_t>(rowHeight);
    }

    FreeTypeFont* FreeTypeConnector::GetOrCreateFont(const std::wstring& fontPath)
    {
        FreeTypeFont* font = nullptr;

        auto it = fFontNameToFont.find(fontPath);
        if (it != fFontNameToFont.end())
        {
            font = it->second.get();
        }
        else
        {
            auto itFontName = fFontNameToFont.emplace(fontPath, std::make_unique<FreeTypeFont>(fLibrary, fontPath));
            font = itFontName.first->second.get();
        }

        return font;
    }


    FT_Stroker FreeTypeConnector::GetStroker()
    {
        if (fStroker == nullptr)
        {
            FT_Stroker_New(fLibrary, &fStroker);
        }
        return fStroker;
    }

    template <typename source_type, typename dest_type>
    void FreeTypeConnector::ResolvePremultipoliedBUffer(LLUtils::Buffer& dest, const LLUtils::Buffer& source, uint32_t width, uint32_t height)
	{
        dest_type* destPtr = reinterpret_cast<dest_type*>(dest.data());
        const source_type* sourcePtr = reinterpret_cast<const source_type*>(source.data());
        for (auto y = 0u ; y < height;y++)
            for (auto x = 0u; x < width; x++)
            {
                destPtr[y * width + x] = static_cast<dest_type>(sourcePtr[y * width + x].DivideAlpha());
            }
	}

    FT_Render_Mode FreeTypeConnector::GetRenderMode(RenderMode renderMode) const
    {
        switch (renderMode)
        {
        case RenderMode::Aliased:
            return FT_Render_Mode::FT_RENDER_MODE_MONO;
        case RenderMode::Default:
        case RenderMode::Antialiased:
            return FT_Render_Mode::FT_RENDER_MODE_NORMAL;
        case RenderMode::SubpixelAntiAliased:
            return FT_Render_Mode::FT_RENDER_MODE_LCD;
        default:
            return FT_Render_Mode::FT_RENDER_MODE_NORMAL;

        }
    }



    void FreeTypeConnector::CreateBitmap(const TextCreateParams& textCreateParams
        , Bitmap& out_bitmap
        , TextMetrics* in_metrics//optional
        , GlyphMappings* out_glyphMapping /*= nullptr*/
            )
    {
        using namespace std;

        const  std::wstring text = textCreateParams.text;
        const std::wstring& fontPath = textCreateParams.fontPath;
        const uint16_t fontSize = textCreateParams.fontSize;
        const uint32_t OutlineWidth = textCreateParams.outlineWidth;
        const LLUtils::Color outlineColor = textCreateParams.outlineColor;
        const LLUtils::Color backgroundColor = textCreateParams.backgroundColor;
        const FT_Render_Mode textRenderMOde = GetRenderMode(textCreateParams.renderMode);

        FreeTypeFont* font = GetOrCreateFont(fontPath);

        font->SetSize(fontSize, textCreateParams.DPIx, textCreateParams.DPIy);

        TextMesureParams params;
        params.createParams = textCreateParams;
        
        TextMetrics metrics;
        if (in_metrics == nullptr)
            MeasureText(params, metrics);
        else
            metrics = *in_metrics;

        
        auto& mesaureResult = metrics;

        using namespace LLUtils;
        const uint32_t destPixelSize = sizeof(ColorF32);
        const uint32_t destRowPitch = mesaureResult.rect.GetWidth() * destPixelSize;
        const uint32_t sizeOfDestBuffer = mesaureResult.rect.GetHeight() * destRowPitch;
        const bool renderOutline = OutlineWidth > 0;
        const bool renderText = true;

        LLUtils::Buffer textBuffer(sizeOfDestBuffer);

        //// when rendering with outline, the outline buffer is the final buffer, otherwise the text buffer is the final buffer.
        //Reset final text buffer to background color.

        ColorF32 textBackgroundBuffer = renderOutline ? ColorF32(0.0f,0.0f,0.0f,0.0f) : static_cast<ColorF32>(backgroundColor).MultiplyAlpha();
        

        for (int32_t i = 0; i < mesaureResult.rect.GetWidth() * mesaureResult.rect.GetHeight(); i++)
            reinterpret_cast<ColorF32*>(textBuffer.data())[i] = textBackgroundBuffer;



        LLUtils::Buffer outlineBuffer;
        BlitBox  destOutline = {};
        if (renderOutline)
        {
            outlineBuffer.Allocate(sizeOfDestBuffer);
            //Reset outline buffer to background color.
            for (int32_t i = 0; i < mesaureResult.rect.GetWidth() * mesaureResult.rect.GetHeight(); i++)
                reinterpret_cast<LLUtils::ColorF32*>(outlineBuffer.data())[i] =  static_cast<ColorF32>(backgroundColor).MultiplyAlpha();

            destOutline.buffer = outlineBuffer.data();
            destOutline.width = static_cast<uint32_t>(mesaureResult.rect.GetWidth());
            destOutline.height = static_cast<uint32_t>(mesaureResult.rect.GetHeight());
            destOutline.pixelSizeInbytes = destPixelSize;
            destOutline.rowPitch = destRowPitch;
        }


        BlitBox  dest {};
        dest.buffer = textBuffer.data();
        dest.width = static_cast<uint32_t>(mesaureResult.rect.GetWidth());
        dest.height = static_cast<uint32_t>(mesaureResult.rect.GetHeight());
        dest.pixelSizeInbytes = destPixelSize;
        dest.rowPitch = destRowPitch;

        
        int penX = -mesaureResult.rect.LeftTop().x;
        int penY = -mesaureResult.rect.LeftTop().y;

        
        FT_Face face = font->GetFace();
        const auto descender = face->size->metrics.descender >> 6;
        const uint32_t rowHeight = mesaureResult.rowHeight;

        vector<FormattedTextEntry> formattedText;
        if ((textCreateParams.flags & TextCreateFlags::UseMetaText) == TextCreateFlags::UseMetaText)
            formattedText = MetaText::GetFormattedText(text);
        else
            formattedText.push_back({ textCreateParams.textColor, textCreateParams.text });



        for (const FormattedTextEntry& el : formattedText)
        {
            const std::u32string visualText = ((textCreateParams.flags & TextCreateFlags::Bidirectional) == TextCreateFlags::Bidirectional)
                ? bidi_string(el.text.c_str()) : ww898::utf::conv<char32_t>(el.text);
            
            for (const decltype(visualText)::value_type& codepoint : visualText)
            {
                if (codepoint == L'\n')
                {
                    penX = static_cast<int>(-mesaureResult.rect.LeftTop().x);
                    penY += rowHeight;
                    continue;
                }

                const FT_UInt glyph_index = FT_Get_Char_Index(face, codepoint);
                if (FT_Error error = FT_Load_Glyph(
                    face,          /* handle to face object */
                    glyph_index,   /* glyph index           */
                    FT_LOAD_DEFAULT); error != FT_Err_Ok)/* load flags, see below */
                {
                    LL_EXCEPTION(LLUtils::Exception::ErrorCode::RuntimeError, GenerateFreeTypeErrorString("can not Load glyph", error));
                }

				
                //Render outline
                const auto advance = face->glyph->advance.x >> 6;
                
                if (textCreateParams.maxWidthPx > 0 && penX + advance + mesaureResult.rect.LeftTop().x > static_cast<int>(textCreateParams.maxWidthPx))
                {
                    penY += rowHeight;
                    penX = static_cast<int>(-mesaureResult.rect.LeftTop().x);

                }

                const auto baseVerticalPos = rowHeight + penY + descender - OutlineWidth;

                if (renderOutline) // render outline
                {
                    //initialize stroker, so you can create outline font
                    FT_Stroker stroker = GetStroker();

                    //  2 * 64 result in 2px outline
                    FT_Stroker_Set(stroker, static_cast<FT_Fixed>(OutlineWidth * 64), FT_STROKER_LINECAP_ROUND, FT_STROKER_LINEJOIN_BEVEL, 0);
                    FT_Glyph glyph;
                    FT_Get_Glyph(face->glyph, &glyph);
                    FT_Glyph_StrokeBorder(&glyph, stroker, false, true);
                    FT_Glyph_To_Bitmap(&glyph, textRenderMOde, nullptr, true);
                    FT_BitmapGlyph bitmapGlyph = reinterpret_cast<FT_BitmapGlyph>(glyph);
                    FreeTypeRenderer::BitmapProperties bitmapProperties = FreeTypeRenderer::GetBitmapGlyphProperties(bitmapGlyph->bitmap);
                    LLUtils::Buffer rasterizedGlyph = FreeTypeRenderer::RenderGlyphToBuffer({ bitmapGlyph , {0,0,0,0} ,outlineColor, bitmapProperties });


                    BlitBox source = {};
                    source.buffer = rasterizedGlyph.data();
                    source.width = bitmapProperties.width;
                    source.height = bitmapProperties.height;
                    source.pixelSizeInbytes = destPixelSize;
                    source.rowPitch = destPixelSize * bitmapProperties.width;

                    destOutline.left = static_cast<uint32_t>(penX + bitmapGlyph->left);
                    destOutline.top = baseVerticalPos - bitmapGlyph->top;
                    BlitBox::BlitPremultiplied<ColorF32>(destOutline, source);

                    FT_Done_Glyph(glyph);

                }
                // Render text
                if (renderText)
                {
                    FT_Glyph glyph;
                    if (FT_Error error = FT_Get_Glyph(face->glyph, &glyph))
                        LL_EXCEPTION(LLUtils::Exception::ErrorCode::RuntimeError, "FreeType error, unable to render glyph");

                    if (glyph->format != FT_GLYPH_FORMAT_BITMAP)
                    {
                        if (FT_Error error = FT_Glyph_To_Bitmap(&glyph, textRenderMOde, nullptr, true); error != FT_Err_Ok)
                            LL_EXCEPTION(LLUtils::Exception::ErrorCode::RuntimeError, "FreeType error, unable to render glyph");
                    }

                    FT_BitmapGlyph bitmapGlyph = reinterpret_cast<FT_BitmapGlyph>(glyph);
                    FreeTypeRenderer::BitmapProperties bitmapProperties = FreeTypeRenderer::GetBitmapGlyphProperties(bitmapGlyph->bitmap);
                    const auto textcolor = el.textColor != Color{0, 0, 0, 0} ? el.textColor : params.createParams.textColor;

                    LLUtils::Buffer rasterizedGlyph = FreeTypeRenderer::RenderGlyphToBuffer({ bitmapGlyph , backgroundColor, textcolor , bitmapProperties });

                    BlitBox source = {};
                    source.buffer = rasterizedGlyph.data();
                    source.width = bitmapProperties.width;
                    source.height = bitmapProperties.height;
                    source.pixelSizeInbytes = destPixelSize;
                    source.rowPitch = destPixelSize * bitmapProperties.width;

                    dest.left = static_cast<uint32_t>(penX +  bitmapGlyph->left);
                    dest.top = baseVerticalPos - bitmapGlyph->top;
                    
                    if (out_glyphMapping != nullptr)
                    {
                        out_glyphMapping->push_back(LLUtils::RectI32{ { penX, penY } ,
                            {penX + static_cast<int32_t>(advance), penY + static_cast<int32_t>(rowHeight)} });
                    }

                    penX += advance;

                    FT_Done_Glyph(glyph);
                    BlitBox::BlitPremultiplied<ColorF32>(dest, source);
                    

                }
            }
        }

        if (renderOutline)
        {
            //Blend text buffer onto outline buffer.
            dest.left = 0;
            dest.top = 0;
            destOutline.left = 0;
            destOutline.top = 0;
            BlitBox::BlitPremultiplied<ColorF32>(destOutline, dest);
        }

        const auto buferToResolve = renderOutline ? std::move(outlineBuffer) : std::move(textBuffer);
        Buffer resolved(mesaureResult.rect.GetWidth() * mesaureResult.rect.GetHeight() * sizeof(Color));
        ResolvePremultipoliedBUffer<ColorF32, Color>(resolved, buferToResolve, static_cast<uint32_t>(mesaureResult.rect.GetWidth()), static_cast<uint32_t>(mesaureResult.rect.GetHeight()));

        out_bitmap.width = static_cast<uint32_t>(mesaureResult.rect.GetWidth());
        out_bitmap.height = static_cast<uint32_t>(mesaureResult.rect.GetHeight());
		
        out_bitmap.buffer =  std::move(resolved);
		
        out_bitmap.PixelSize = sizeof(Color);
        out_bitmap.rowPitch = sizeof(Color) * mesaureResult.rect.GetWidth();

    }
}