/*
 *  Copyright (C) 2006, 2007, 2008, 2009, 2010, 2011, 2012, 2013 Stephen F. Booth <me@sbooth.org>
 *  All Rights Reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are
 *  met:
 *
 *    - Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    - Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *    - Neither the name of Stephen F. Booth nor the names of its 
 *      contributors may be used to endorse or promote products derived
 *      from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *  HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <AudioToolbox/AudioFormat.h>
#include <CoreFoundation/CoreFoundation.h>

#include "HTTPInputSource.h"
#include "AudioDecoder.h"
#include "Logger.h"
#include "CFWrapper.h"
#include "CFErrorUtilities.h"
#include "CreateStringForOSType.h"
#include "LoopableRegionDecoder.h"

// ========================================
// Error Codes
// ========================================
const CFStringRef SFB::Audio::Decoder::ErrorDomain = CFSTR("org.sbooth.AudioEngine.ErrorDomain.AudioDecoder");

#pragma mark Static Methods

std::atomic_bool SFB::Audio::Decoder::sAutomaticallyOpenDecoders = ATOMIC_VAR_INIT(false);
std::vector<SFB::Audio::Decoder::SubclassInfo> SFB::Audio::Decoder::sRegisteredSubclasses;

CFArrayRef SFB::Audio::Decoder::CreateSupportedFileExtensions()
{
	CFMutableArrayRef supportedFileExtensions = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);

	for(auto subclassInfo : sRegisteredSubclasses) {
		SFB::CFArray decoderFileExtensions = subclassInfo.mCreateSupportedFileExtensions();
		CFArrayAppendArray(supportedFileExtensions, decoderFileExtensions, CFRangeMake(0, CFArrayGetCount(decoderFileExtensions)));
	}

	return supportedFileExtensions;
}

CFArrayRef SFB::Audio::Decoder::CreateSupportedMIMETypes()
{
	CFMutableArrayRef supportedMIMETypes = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);

	for(auto subclassInfo : sRegisteredSubclasses) {
		SFB::CFArray decoderMIMETypes = subclassInfo.mCreateSupportedMIMETypes();
		CFArrayAppendArray(supportedMIMETypes, decoderMIMETypes, CFRangeMake(0, CFArrayGetCount(decoderMIMETypes)));
	}

	return supportedMIMETypes;
}

bool SFB::Audio::Decoder::HandlesFilesWithExtension(CFStringRef extension)
{
	if(nullptr == extension)
		return false;

	for(auto subclassInfo : sRegisteredSubclasses) {
		if(subclassInfo.mHandlesFilesWithExtension(extension))
			return true;
	}

	return false;
}

bool SFB::Audio::Decoder::HandlesMIMEType(CFStringRef mimeType)
{
	if(nullptr == mimeType)
		return false;

	for(auto subclassInfo : sRegisteredSubclasses) {
		if(subclassInfo.mHandlesMIMEType(mimeType))
			return true;
	}

	return false;
}

SFB::Audio::Decoder::unique_ptr SFB::Audio::Decoder::CreateDecoderForURL(CFURLRef url, CFErrorRef *error)
{
	return CreateDecoderForURL(url, nullptr, error);
}

SFB::Audio::Decoder::unique_ptr SFB::Audio::Decoder::CreateDecoderForURL(CFURLRef url, CFStringRef mimeType, CFErrorRef *error)
{
	return CreateDecoderForInputSource(InputSource::CreateInputSourceForURL(url, 0, error), mimeType, error);
}

SFB::Audio::Decoder::unique_ptr SFB::Audio::Decoder::CreateDecoderForInputSource(InputSource::unique_ptr inputSource, CFErrorRef *error)
{
	return CreateDecoderForInputSource(std::move(inputSource), nullptr, error);
}

SFB::Audio::Decoder::unique_ptr SFB::Audio::Decoder::CreateDecoderForInputSource(InputSource::unique_ptr inputSource, CFStringRef mimeType, CFErrorRef *error)
{
	if(!inputSource)
		return nullptr;

	// Open the input source if it isn't already
	if(AutomaticallyOpenDecoders() && !inputSource->IsOpen() && !inputSource->Open(error))
		return nullptr;

#if 0
	// If the input is an instance of HTTPInputSource, use the MIME type from the server
	// This code is disabled because most HTTP servers don't send the correct MIME types
	HTTPInputSource *httpInputSource = dynamic_cast<HTTPInputSource *>(inputSource);
	bool releaseMIMEType = false;
	if(!mimeType && httpInputSource && httpInputSource->IsOpen()) {
		mimeType = httpInputSource->CopyContentMIMEType();
		if(mimeType)
			releaseMIMEType = true;
	}
#endif

	// The MIME type takes precedence over the file extension
	if(mimeType) {
		for(auto subclassInfo : sRegisteredSubclasses) {
			if(subclassInfo.mHandlesMIMEType(mimeType)) {
				unique_ptr decoder(subclassInfo.mCreateDecoder(std::move(inputSource)));
				if(!AutomaticallyOpenDecoders())
					return decoder;
				else {
					 if(decoder->Open(error))
						 return decoder;
					// Take back the input source for reuse if opening fails
					else
						 inputSource = std::move(decoder->mInputSource);
				}
			}
		}

#if 0
		if(releaseMIMEType)
			CFRelease(mimeType), mimeType = nullptr;
#endif
	}

	// If no MIME type was specified, use the extension-based resolvers

	CFURLRef inputURL = inputSource->GetURL();
	if(!inputURL)
		return nullptr;

	SFB::CFString pathExtension = CFURLCopyPathExtension(inputURL);
	if(!pathExtension) {
		if(error) {
			SFB::CFString description = CFCopyLocalizedString(CFSTR("The type of the file “%@” could not be determined."), "");
			SFB::CFString failureReason = CFCopyLocalizedString(CFSTR("Unknown file type"), "");
			SFB::CFString recoverySuggestion = CFCopyLocalizedString(CFSTR("The file's extension may be missing or may not match the file's type."), "");
			
			*error = CreateErrorForURL(InputSource::ErrorDomain, InputSource::FileNotFoundError, description, inputURL, failureReason, recoverySuggestion);
		}

		return nullptr;
	}

	// TODO: Some extensions (.oga for example) support multiple audio codecs (Vorbis, FLAC, Speex)
	// and if openDecoder is false the wrong decoder type may be returned, since the file isn't analyzed
	// until Open() is called

	for(auto subclassInfo : sRegisteredSubclasses) {
		if(subclassInfo.mHandlesFilesWithExtension(pathExtension)) {
			unique_ptr decoder(subclassInfo.mCreateDecoder(std::move(inputSource)));
			if(!AutomaticallyOpenDecoders())
				return decoder;
			else {
				if(decoder->Open(error))
					return decoder;
				// Take back the input source for reuse if opening fails
				else
					inputSource = std::move(decoder->mInputSource);
			}
		}
	}

	return nullptr;
}

SFB::Audio::Decoder::unique_ptr SFB::Audio::Decoder::CreateDecoderForURLRegion(CFURLRef url, SInt64 startingFrame, CFErrorRef *error)
{
	return CreateDecoderForInputSourceRegion(InputSource::CreateInputSourceForURL(url, 0, error), startingFrame, error);
}

SFB::Audio::Decoder::unique_ptr SFB::Audio::Decoder::CreateDecoderForURLRegion(CFURLRef url, SInt64 startingFrame, UInt32 frameCount, CFErrorRef *error)
{
	return CreateDecoderForInputSourceRegion(InputSource::CreateInputSourceForURL(url, 0, error), startingFrame, frameCount, error);
}

SFB::Audio::Decoder::unique_ptr SFB::Audio::Decoder::CreateDecoderForURLRegion(CFURLRef url, SInt64 startingFrame, UInt32 frameCount, UInt32 repeatCount, CFErrorRef *error)
{
	return CreateDecoderForInputSourceRegion(InputSource::CreateInputSourceForURL(url, 0, error), startingFrame, frameCount, repeatCount, error);
}

SFB::Audio::Decoder::unique_ptr SFB::Audio::Decoder::CreateDecoderForInputSourceRegion(InputSource::unique_ptr inputSource, SInt64 startingFrame, CFErrorRef *error)
{
	if(!inputSource)
		return nullptr;

	return CreateDecoderForDecoderRegion(CreateDecoderForInputSource(std::move(inputSource), error), startingFrame, error);
}

SFB::Audio::Decoder::unique_ptr SFB::Audio::Decoder::CreateDecoderForInputSourceRegion(InputSource::unique_ptr inputSource, SInt64 startingFrame, UInt32 frameCount, CFErrorRef *error)
{
	if(!inputSource)
		return nullptr;

	return CreateDecoderForDecoderRegion(CreateDecoderForInputSource(std::move(inputSource), error), startingFrame, frameCount, error);
}

SFB::Audio::Decoder::unique_ptr SFB::Audio::Decoder::CreateDecoderForInputSourceRegion(InputSource::unique_ptr inputSource, SInt64 startingFrame, UInt32 frameCount, UInt32 repeatCount, CFErrorRef *error)
{
	if(!inputSource)
		return nullptr;

	return CreateDecoderForDecoderRegion(CreateDecoderForInputSource(std::move(inputSource), error), startingFrame, frameCount, repeatCount, error);
}

SFB::Audio::Decoder::unique_ptr SFB::Audio::Decoder::CreateDecoderForDecoderRegion(Decoder::unique_ptr decoder, SInt64 startingFrame, CFErrorRef */*error*/)
{
	if(!decoder)
		return nullptr;
	
	return unique_ptr(new LoopableRegionDecoder(std::move(decoder), startingFrame));
}

SFB::Audio::Decoder::unique_ptr SFB::Audio::Decoder::CreateDecoderForDecoderRegion(Decoder::unique_ptr decoder, SInt64 startingFrame, UInt32 frameCount, CFErrorRef */*error*/)
{
	if(!decoder)
		return nullptr;

	return unique_ptr(new LoopableRegionDecoder(std::move(decoder), startingFrame, frameCount));
}

SFB::Audio::Decoder::unique_ptr SFB::Audio::Decoder::CreateDecoderForDecoderRegion(Decoder::unique_ptr decoder, SInt64 startingFrame, UInt32 frameCount, UInt32 repeatCount, CFErrorRef *)
{
	if(!decoder)
		return nullptr;

	return unique_ptr(new LoopableRegionDecoder(std::move(decoder), startingFrame, frameCount, repeatCount));
}

#pragma mark Creation and Destruction

SFB::Audio::Decoder::Decoder()
	: mInputSource(nullptr), mIsOpen(false), mRepresentedObject(nullptr)
{
	memset(&mFormat, 0, sizeof(mFormat));
	memset(&mSourceFormat, 0, sizeof(mSourceFormat));
}

SFB::Audio::Decoder::Decoder(InputSource::unique_ptr inputSource)
	: mInputSource(std::move(inputSource)), mIsOpen(false), mRepresentedObject(nullptr)
{
	assert(nullptr != mInputSource);

	memset(&mFormat, 0, sizeof(mFormat));
	memset(&mSourceFormat, 0, sizeof(mSourceFormat));
}

#pragma mark Base Functionality

bool SFB::Audio::Decoder::Open(CFErrorRef *error)
{
	if(IsOpen()) {
		LOGGER_INFO("org.sbooth.AudioEngine.Decoder", "Open() called on a Decoder that is already open");
		return true;
	}

	// Ensure the input source is open
	if(!GetInputSource().IsOpen() && !GetInputSource().Open(error))
		return false;

	bool result = _Open(error);
	if(result)
		mIsOpen = true;
	return result;
}

bool SFB::Audio::Decoder::Close(CFErrorRef *error)
{
	if(!IsOpen()) {
		LOGGER_INFO("org.sbooth.AudioEngine.Decoder", "Close() called on a Decoder that hasn't been opened");
		return true;
	}

	// Close the decoder
	bool result = _Close(error);
	if(result)
		mIsOpen = false;

	// Close the input source
	if(!GetInputSource().Close(error))
		return false;

	return result;
}

CFStringRef SFB::Audio::Decoder::CreateFormatDescription() const
{
	if(!IsOpen()) {
		LOGGER_INFO("org.sbooth.AudioEngine.Decoder", "CreateFormatDescription() called on a Decoder that hasn't been opened");
		return nullptr;
	}

	CFStringRef		sourceFormatDescription		= nullptr;
	UInt32			specifierSize				= sizeof(sourceFormatDescription);
	OSStatus		result						= AudioFormatGetProperty(kAudioFormatProperty_FormatName,
																		 sizeof(mFormat),
																		 &mFormat,
																		 &specifierSize,
																		 &sourceFormatDescription);

	if(noErr != result)
		LOGGER_ERR("org.sbooth.AudioEngine.Decoder", "AudioFormatGetProperty (kAudioFormatProperty_FormatName) failed: " << result << "'" << SFB::StringForOSType((OSType)result) << "'");

	return sourceFormatDescription;
}

CFStringRef SFB::Audio::Decoder::CreateSourceFormatDescription() const
{
	if(!IsOpen()) {
		LOGGER_INFO("org.sbooth.AudioEngine.Decoder", "CreateSourceFormatDescription() called on a Decoder that hasn't been opened");
		return nullptr;
	}

	return _GetSourceFormatDescription().Relinquish();
}

CFStringRef SFB::Audio::Decoder::CreateChannelLayoutDescription() const
{
	if(!IsOpen()) {
		LOGGER_INFO("org.sbooth.AudioEngine.Decoder", "CreateChannelLayoutDescription() called on a Decoder that hasn't been opened");
		return nullptr;
	}

	CFStringRef		channelLayoutDescription	= nullptr;
	UInt32			specifierSize				= sizeof(channelLayoutDescription);
	OSStatus		result						= AudioFormatGetProperty(kAudioFormatProperty_ChannelLayoutName, 
																		 sizeof(mChannelLayout.GetACL()),
																		 mChannelLayout.GetACL(),
																		 &specifierSize, 
																		 &channelLayoutDescription);

	if(noErr != result)
		LOGGER_ERR("org.sbooth.AudioEngine.Decoder", "AudioFormatGetProperty (kAudioFormatProperty_ChannelLayoutName) failed: " << result << "'" << SFB::StringForOSType((OSType)result) << "'");

	return channelLayoutDescription;
}

UInt32 SFB::Audio::Decoder::ReadAudio(AudioBufferList *bufferList, UInt32 frameCount)
{
	if(!IsOpen()) {
		LOGGER_INFO("org.sbooth.AudioEngine.Decoder", "ReadAudio() called on a Decoder that hasn't been opened");
		return 0;
	}

	if(nullptr == bufferList || 0 == frameCount) {
		LOGGER_WARNING("org.sbooth.AudioEngine.Decoder", "ReadAudio() called with invalid parameters");
		return 0;
	}

	return _ReadAudio(bufferList, frameCount);
}

SInt64 SFB::Audio::Decoder::GetTotalFrames() const
{
	if(!IsOpen()) {
		LOGGER_INFO("org.sbooth.AudioEngine.Decoder", "GetTotalFrames() called on a Decoder that hasn't been opened");
		return -1;
	}

	return _GetTotalFrames();
}

SInt64 SFB::Audio::Decoder::GetCurrentFrame() const
{
	if(!IsOpen()) {
		LOGGER_INFO("org.sbooth.AudioEngine.Decoder", "GetCurrentFrame() called on a Decoder that hasn't been opened");
		return -1;
	}

	return _GetCurrentFrame();
}

bool SFB::Audio::Decoder::SupportsSeeking() const
{
	if(!IsOpen()) {
		LOGGER_INFO("org.sbooth.AudioEngine.Decoder", "SupportsSeeking() called on a Decoder that hasn't been opened");
		return false;
	}

	return _SupportsSeeking();
}

SInt64 SFB::Audio::Decoder::SeekToFrame(SInt64 frame)
{
	if(!IsOpen()) {
		LOGGER_INFO("org.sbooth.AudioEngine.Decoder", "SeekToFrame() called on a Decoder that hasn't been opened");
		return -1;
	}

	if(0 > frame || frame >= GetTotalFrames()) {
		LOGGER_WARNING("org.sbooth.AudioEngine.Decoder", "SeekToFrame() called with invalid parameters");
		return -1;
	}

	return _SeekToFrame(frame);
}
