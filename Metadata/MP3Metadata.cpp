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

#include <memory>

#include <taglib/tfilestream.h>
#include <taglib/mpegfile.h>
#include <taglib/mpegproperties.h>
#include <taglib/xingheader.h>

#include "MP3Metadata.h"
#include "CFWrapper.h"
#include "CFErrorUtilities.h"
#include "AddAPETagToDictionary.h"
#include "AddID3v1TagToDictionary.h"
#include "AddID3v2TagToDictionary.h"
#include "SetID3v1TagFromMetadata.h"
#include "SetID3v2TagFromMetadata.h"
#include "SetAPETagFromMetadata.h"
#include "AddAudioPropertiesToDictionary.h"
#include "CFDictionaryUtilities.h"

namespace {

	void RegisterMP3Metadata() __attribute__ ((constructor));
	void RegisterMP3Metadata()
	{
		SFB::Audio::Metadata::RegisterSubclass<SFB::Audio::MP3Metadata>();
	}

}

#pragma mark Static Methods

CFArrayRef SFB::Audio::MP3Metadata::CreateSupportedFileExtensions()
{
	CFStringRef supportedExtensions [] = { CFSTR("mp3") };
	return CFArrayCreate(kCFAllocatorDefault, (const void **)supportedExtensions, 1, &kCFTypeArrayCallBacks);
}

CFArrayRef SFB::Audio::MP3Metadata::CreateSupportedMIMETypes()
{
	CFStringRef supportedMIMETypes [] = { CFSTR("audio/mpeg") };
	return CFArrayCreate(kCFAllocatorDefault, (const void **)supportedMIMETypes, 1, &kCFTypeArrayCallBacks);
}

bool SFB::Audio::MP3Metadata::HandlesFilesWithExtension(CFStringRef extension)
{
	if(nullptr == extension)
		return false;
	
	if(kCFCompareEqualTo == CFStringCompare(extension, CFSTR("mp3"), kCFCompareCaseInsensitive))
		return true;

	return false;
}

bool SFB::Audio::MP3Metadata::HandlesMIMEType(CFStringRef mimeType)
{
	if(nullptr == mimeType)
		return false;
	
	if(kCFCompareEqualTo == CFStringCompare(mimeType, CFSTR("audio/mpeg"), kCFCompareCaseInsensitive))
		return true;
	
	return false;
}

SFB::Audio::Metadata::unique_ptr SFB::Audio::MP3Metadata::CreateMetadata(CFURLRef url)
{
	return unique_ptr(new MP3Metadata(url));
}

#pragma mark Creation and Destruction

SFB::Audio::MP3Metadata::MP3Metadata(CFURLRef url)
	: AudioMetadata(url)
{}

#pragma mark Functionality

bool SFB::Audio::MP3Metadata::_ReadMetadata(CFErrorRef *error)
{
	UInt8 buf [PATH_MAX];
	if(!CFURLGetFileSystemRepresentation(mURL, false, buf, PATH_MAX))
		return false;
	
	std::unique_ptr<TagLib::FileStream> stream(new TagLib::FileStream((const char *)buf, true));
	if(!stream->isOpen()) {
		if(error) {
			SFB::CFString description = CFCopyLocalizedString(CFSTR("The file “%@” could not be opened for reading."), "");
			SFB::CFString failureReason = CFCopyLocalizedString(CFSTR("Input/output error"), "");
			SFB::CFString recoverySuggestion = CFCopyLocalizedString(CFSTR("The file may have been renamed, moved, deleted, or you may not have appropriate permissions."), "");

			*error = CreateErrorForURL(Metadata::ErrorDomain, Metadata::InputOutputError, description, mURL, failureReason, recoverySuggestion);
		}

		return false;
	}

	TagLib::MPEG::File file(stream.get(), TagLib::ID3v2::FrameFactory::instance());
	if(!file.isValid()) {
		if(nullptr != error) {
			SFB::CFString description = CFCopyLocalizedString(CFSTR("The file “%@” is not a valid MPEG file."), "");
			SFB::CFString failureReason = CFCopyLocalizedString(CFSTR("Not an MPEG file"), "");
			SFB::CFString recoverySuggestion = CFCopyLocalizedString(CFSTR("The file's extension may not match the file's type."), "");
			
			*error = CreateErrorForURL(Metadata::ErrorDomain, Metadata::InputOutputError, description, mURL, failureReason, recoverySuggestion);
		}
		
		return false;
	}
	
	CFDictionarySetValue(mMetadata, kFormatNameKey, CFSTR("MP3"));

	if(file.audioProperties()) {
		auto properties = file.audioProperties();
		AddAudioPropertiesToDictionary(mMetadata, properties);

		// TODO: Is this too much information?
#if 0
		switch(properties->version()) {
			case TagLib::MPEG::Header::Version1:
				switch(properties->layer()) {
					case 1:		CFDictionarySetValue(mMetadata, kFormatNameKey, CFSTR("MPEG-1 Layer I"));		break;
					case 2:		CFDictionarySetValue(mMetadata, kFormatNameKey, CFSTR("MPEG-1 Layer II"));	break;
					case 3:		CFDictionarySetValue(mMetadata, kFormatNameKey, CFSTR("MPEG-1 Layer III"));	break;
				}
				break;
			case TagLib::MPEG::Header::Version2:
				switch(properties->layer()) {
					case 1:		CFDictionarySetValue(mMetadata, kFormatNameKey, CFSTR("MPEG-2 Layer I"));		break;
					case 2:		CFDictionarySetValue(mMetadata, kFormatNameKey, CFSTR("MPEG-2 Layer II"));	break;
					case 3:		CFDictionarySetValue(mMetadata, kFormatNameKey, CFSTR("MPEG-2 Layer III"));	break;
				}
				break;
			case TagLib::MPEG::Header::Version2_5:
				switch(properties->layer()) {
					case 1:		CFDictionarySetValue(mMetadata, kFormatNameKey, CFSTR("MPEG-2.5 Layer I"));	break;
					case 2:		CFDictionarySetValue(mMetadata, kFormatNameKey, CFSTR("MPEG-2.5 Layer II"));	break;
					case 3:		CFDictionarySetValue(mMetadata, kFormatNameKey, CFSTR("MPEG-2.5 Layer III"));	break;
				}
				break;
		}
#endif

		if(properties->xingHeader() && properties->xingHeader()->totalFrames())
			AddIntToDictionary(mMetadata, kTotalFramesKey, (int)properties->xingHeader()->totalFrames());
	}

	if(file.APETag())
		AddAPETagToDictionary(mMetadata, mPictures, file.APETag());

	if(file.ID3v1Tag())
		AddID3v1TagToDictionary(mMetadata, file.ID3v1Tag());

	if(file.ID3v2Tag())
		AddID3v2TagToDictionary(mMetadata, mPictures, file.ID3v2Tag());

	return true;
}

bool SFB::Audio::MP3Metadata::_WriteMetadata(CFErrorRef *error)
{
	UInt8 buf [PATH_MAX];
	if(!CFURLGetFileSystemRepresentation(mURL, false, buf, PATH_MAX))
		return false;

	std::unique_ptr<TagLib::FileStream> stream(new TagLib::FileStream((const char *)buf));
	if(!stream->isOpen()) {
		if(error) {
			SFB::CFString description = CFCopyLocalizedString(CFSTR("The file “%@” could not be opened for writing."), "");
			SFB::CFString failureReason = CFCopyLocalizedString(CFSTR("Input/output error"), "");
			SFB::CFString recoverySuggestion = CFCopyLocalizedString(CFSTR("The file may have been renamed, moved, deleted, or you may not have appropriate permissions."), "");

			*error = CreateErrorForURL(Metadata::ErrorDomain, Metadata::InputOutputError, description, mURL, failureReason, recoverySuggestion);
		}

		return false;
	}

	TagLib::MPEG::File file(stream.get(), TagLib::ID3v2::FrameFactory::instance(), false);
	if(!file.isValid()) {
		if(error) {
			SFB::CFString description = CFCopyLocalizedString(CFSTR("The file “%@” is not a valid MPEG file."), "");
			SFB::CFString failureReason = CFCopyLocalizedString(CFSTR("Not an MPEG file"), "");
			SFB::CFString recoverySuggestion = CFCopyLocalizedString(CFSTR("The file's extension may not match the file's type."), "");
			
			*error = CreateErrorForURL(Metadata::ErrorDomain, Metadata::InputOutputError, description, mURL, failureReason, recoverySuggestion);
		}
		
		return false;
	}

	// APE and ID3v1 tags are only written if present, but ID3v2 tags are always written

	auto APETag = file.APETag();
	if(APETag && !APETag->isEmpty())
		SetAPETagFromMetadata(*this, APETag);

	auto ID3v1Tag = file.ID3v1Tag();
	if(ID3v1Tag && !ID3v1Tag->isEmpty())
		SetID3v1TagFromMetadata(*this, ID3v1Tag);

	SetID3v2TagFromMetadata(*this, file.ID3v2Tag(true));

	if(!file.save()) {
		if(error) {
			SFB::CFString description = CFCopyLocalizedString(CFSTR("The file “%@” is not a valid MPEG file."), "");
			SFB::CFString failureReason = CFCopyLocalizedString(CFSTR("Unable to write metadata"), "");
			SFB::CFString recoverySuggestion = CFCopyLocalizedString(CFSTR("The file's extension may not match the file's type."), "");
			
			*error = CreateErrorForURL(Metadata::ErrorDomain, Metadata::InputOutputError, description, mURL, failureReason, recoverySuggestion);
		}
		
		return false;
	}

	return true;
}
