/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "udm.hpp"
#include <sharedutils/base64.hpp>
#include <sstream>

#pragma optimize("",off)
namespace udm
{
	class AsciiReader
	{
	public:
		static std::shared_ptr<udm::Data> LoadAscii(const VFilePtr &f);
	private:
		enum class BlockResult : uint8_t
		{
			EndOfBlock = 0,
			EndOfFile
		};
		template<class TException>
			TException BuildException(const std::string &msg)
			{return TException{msg,m_curLine,(m_curCharPos > 0) ? (m_curCharPos -1) : 0};}
		char PeekNextChar();
		char ReadUntil(char c,std::string *optOut=nullptr);
		char ReadNextToken();
		void MoveCursorForward(uint32_t n);
		void SeekNextToken(const std::optional<char> &tSeek={});
		std::string ReadString();
		std::string ReadString(char initialC);
		void ReadValue(Type type,void *outData);
		void ReadStructValue(const StructDescription &strct,void *outData);
		void ReadValueList(Type type,const std::function<bool()> &valueHandler,bool enableSubLists=true);
		void ReadTemplateParameterList(std::vector<Type> &outTypes,std::vector<std::string> &outNames);
		template<typename T,typename TBase>
			void ReadTypedValueList(T &outData);
		template<typename T>
			void ReadFloatValueList(T &outData);
		void ReadBlobData(std::vector<uint8_t> &outData);
		BlockResult ReadBlockKeyValues(Element &parent);
		char ReadChar();

		VFilePtr m_file;
		uint32_t m_curCharPos = 0; // Current character relative to current line
		uint32_t m_curLine = 0;
	};
};

template<typename T>
	static T to_int(const std::string &str) {return static_cast<T>(util::to_int(str));}

udm::AsciiException::AsciiException(const std::string &msg,uint32_t lineIdx,uint32_t charIdx)
	: Exception{msg +" in line " +std::to_string(lineIdx +1) +" (column " +std::to_string(charIdx +1) +")"},
	lineIndex{lineIdx},charIndex{charIdx}
{}

udm::Type udm::ascii_type_to_enum(const std::string &type)
{
	// Note: These have to match enum_type_to_ascii
	static std::unordered_map<std::string,Type> namedTypeToEnum = {
		{"nil",Type::Nil},
		{"string",Type::String},
		{"utf8",Type::Utf8String},
		{"int8",Type::Int8},
		{"uint8",Type::UInt8},
		{"int16",Type::Int16},
		{"uint16",Type::UInt16},
		{"int32",Type::Int32},
		{"uint32",Type::UInt32},
		{"int64",Type::Int64},
		{"uint64",Type::UInt64},
		{"float",Type::Float},
		{"double",Type::Double},
		{"bool",Type::Boolean},
		{"vec2",Type::Vector2},
		{"vec2i",Type::Vector2i},
		{"vec3",Type::Vector3},
		{"vec3i",Type::Vector3i},
		{"vec4",Type::Vector4},
		{"vec4i",Type::Vector4i},
		{"quat",Type::Quaternion},
		{"ang",Type::EulerAngles},
		{"srgba",Type::Srgba},
		{"hdr",Type::HdrColor},
		{"transform",Type::Transform},
		{"stransform",Type::ScaledTransform},
		{"mat4",Type::Mat4},
		{"mat3x4",Type::Mat3x4},
		{"blob",Type::Blob},
		{"lz4",Type::BlobLz4},
		{"array",Type::Array},
		{"arrayLz4",Type::ArrayLz4},
		{"element",Type::Element},
		{"ref",Type::Reference},
		{"struct",Type::Struct},
		{"half",Type::Half}
	};
	static_assert(umath::to_integral(Type::Count) == 36,"Update this list when new types are added!");
	auto it = namedTypeToEnum.find(type);
	return (it != namedTypeToEnum.end()) ? it->second : Type::Invalid;
}

void udm::sanitize_key_name(std::string &key)
{
	ustring::replace(key,".","");
}

template<typename T>
	void udm::AsciiReader::ReadFloatValueList(T &outData)
{
	ReadTypedValueList<T,float>(outData);
}

void udm::AsciiReader::ReadTemplateParameterList(std::vector<Type> &outTypes,std::vector<std::string> &outNames)
{
	auto t = ReadNextToken();
	if(t != '<')
		throw BuildException<SyntaxError>("Expected '<' to initiate template parameter list, got '" +std::string{t} +"'");
	SeekNextToken();
	t = PeekNextChar();
	for(;;)
	{
		switch(t)
		{
		case '>':
		{
			if(outTypes.empty())
				throw BuildException<SyntaxError>("Structs with empty template parameter lists are not allowed, at least one type has to be specified!");
			MoveCursorForward(1);
			return;
		}
		case ':':
			throw BuildException<SyntaxError>("Unexpected token '" +std::string{t} +"'");
			return;
		case EOF:
			throw BuildException<SyntaxError>("Unexpected EOF");
			return;
		}
		if(!outTypes.empty())
		{
			if(t != ',')
				throw BuildException<SyntaxError>("Unexpected token '" +std::string{t} +"'");
			MoveCursorForward(1);
		}

		auto stype = ReadString();
		auto type = ascii_type_to_enum(stype);
		if(type == Type::Invalid)
			throw BuildException<SyntaxError>("Invalid type '" +stype +"' specified in template parameter list!");
		if(!is_trivial_type(type))
			throw BuildException<SyntaxError>("Non-trivial type '" +stype +"' specified in template parameter list, only trivial types are allowed!");
		outTypes.push_back(type);

		SeekNextToken();
		t = PeekNextChar();
		if(t == ':')
		{
			MoveCursorForward(1);
			outNames.push_back(ReadString());
			t = PeekNextChar();
		}
		else
			outNames.push_back("");
	}
}

void udm::AsciiReader::ReadValueList(Type type,const std::function<bool()> &valueHandler,bool enableSubLists)
{
	auto t = ReadNextToken();
	if(t != '[')
		throw BuildException<SyntaxError>("Expected '[' to initiate value list, got '" +std::string{t} +"'");
	uint32_t depth = 1;
	for(;;)
	{
		t = ReadNextToken();
		if(t == '[' && enableSubLists)
		{
			if(type != Type::Array)
			{
				++depth;
				continue;
			}
		}
		else if(t == ']')
		{
			assert(depth > 0);
			if(--depth == 0)
				return;
			continue;
		}
		else if(t == EOF)
			throw BuildException<SyntaxError>("Unexpected EOF");
		if(t != ',')
		{
			assert(m_curCharPos > 0);
			--m_curCharPos;
			m_file->Seek(m_file->Tell() -1);
		}
		else
			SeekNextToken();
		if(valueHandler() == false)
			throw BuildException<DataError>("Invalid value for type '" +std::string{enum_type_to_ascii(type)} +"'");
	}
}

void udm::AsciiReader::ReadBlobData(std::vector<uint8_t> &outData)
{
	auto t = ReadNextToken();
	if(t != '[')
		throw BuildException<SyntaxError>("Expected '[' to initiate blob data, got '" +std::string{t} +"'");
	auto start = m_file->Tell();
	ReadUntil(']');
	auto end = m_file->Tell();
	auto blobSize = end -start -1;
	std::string strData;
	m_file->Seek(start);
	strData.resize(blobSize);
	auto numRead = m_file->Read(strData.data(),blobSize);
	if(numRead != blobSize)
		throw BuildException<SyntaxError>("Unexpected end of blob data");
	// TODO: Optimize this so no copy is required!
	try
	{
		auto decoded = util::base64_decode(strData);
		outData.resize(decoded.size());
		memcpy(outData.data(),decoded.data(),outData.size());
	}
	catch(const std::runtime_error &e)
	{
		throw BuildException<DataError>(e.what());
	}
	MoveCursorForward(1); // Move past ']'
}

constexpr bool is_float_based_type(udm::Type type)
{
	switch(type)
	{
	case udm::Type::Vector2:
	case udm::Type::Vector3:
	case udm::Type::Vector4:
	case udm::Type::Quaternion:
	case udm::Type::EulerAngles:
	case udm::Type::Transform:
	case udm::Type::ScaledTransform:
	case udm::Type::Mat4:
	case udm::Type::Mat3x4:
		return true;
	}
	static_assert(umath::to_integral(udm::Type::Count) == 36,"Update this list when new types are added!");
	return false;
}

void udm::AsciiReader::ReadStructValue(const StructDescription &strct,void *outData)
{
	auto &types = strct.types;
	auto &names = strct.names;
	auto t = ReadNextToken();
	if(t != '[')
		throw BuildException<SyntaxError>("Expected '[' to initiate value list, got '" +std::string{t} +"'");
	uint64_t offset = 0;
	auto *ptr = static_cast<uint8_t*>(outData);
	for(auto i=decltype(types.size()){0u};i<types.size();++i)
	{
		if(i > 0)
		{
			t = ReadNextToken();
			if(t != ',')
				throw BuildException<SyntaxError>("Expected ',' to continue value list, got '" +std::string{t} +"'");
		}
		auto type = types[i];
		ReadValue(type,ptr +offset);
		offset += size_of(type);
	}
	t = ReadNextToken();
	if(t != ']')
		throw BuildException<SyntaxError>("Expected ']' to close value list, got '" +std::string{t} +"'");
}

void udm::AsciiReader::ReadValue(Type type,void *outData)
{
	if(is_float_based_type(type))
	{
		std::visit([this,&outData](auto tag) {
			using T = decltype(tag)::type;
			auto &v = *static_cast<T*>(outData);
			if constexpr(is_float_based_type(type_to_enum_s<T>())) // Always true in this context!
				ReadFloatValueList(v);
			// Note: Quaternions are stored in the Ascii format in w-x-y-z order, but their binary data
			// in glm is stored as x-y-z-w, so we have to re-order the components here.
			auto fixQuatComponentOrder = [](Quat &v) {
				v = {v.x,v.y,v.z,v.w};
			};
			if constexpr(std::is_same_v<T,Quaternion>)
				fixQuatComponentOrder(v);
			else if constexpr(std::is_same_v<T,Transform> || std::is_same_v<T,ScaledTransform>)
				fixQuatComponentOrder(v.GetRotation());
		},get_generic_tag(type));
		return;
	}
	switch(type)
	{
	case Type::Nil:
		break;
	case Type::String:
	{
		*static_cast<String*>(outData) = ReadString();
		break;
	}
	case Type::Reference:
	{
		static_cast<Reference*>(outData)->path = ReadString();
		break;
	}
	case Type::Utf8String:
	{
		auto &str = *static_cast<Utf8String*>(outData);
		ReadValueList(type,[this]() -> bool {
			std::string str;
			ReadValue(udm::Type::String,&str);
			return str == "base64";
		});

		ReadBlobData(str.data);
		break;
	}
	case Type::Int8:
		*static_cast<Int8*>(outData) = to_int<Int8>(ReadString());
		break;
	case Type::UInt8:
		*static_cast<UInt8*>(outData) = to_int<UInt8>(ReadString());
		break;
	case Type::Int16:
		*static_cast<Int16*>(outData) = to_int<Int16>(ReadString());
		break;
	case Type::UInt16:
		*static_cast<UInt16*>(outData) = to_int<UInt16>(ReadString());
		break;
	case Type::Int32:
		*static_cast<Int32*>(outData) = to_int<Int32>(ReadString());
		break;
	case Type::UInt32:
		*static_cast<UInt32*>(outData) = static_cast<UInt32>(util::to_uint64(ReadString()));
		break;
	case Type::Int64:
		*static_cast<Int64*>(outData) = static_cast<Int64>(atoll(ReadString().c_str()));
		break;
	case Type::UInt64:
		*static_cast<UInt64*>(outData) = static_cast<UInt64>(util::to_uint64(ReadString()));
		break;
	case Type::Float:
		*static_cast<Float*>(outData) = util::to_float(ReadString());
		break;
	case Type::Double:
		*static_cast<Double*>(outData) = static_cast<Double>(stod(ReadString()));
		break;
	case Type::Boolean:
		*static_cast<Boolean*>(outData) = util::to_boolean(ReadString());
		break;
	case Type::Srgba:
		ReadTypedValueList<Srgba,Srgba::value_type>(*static_cast<Srgba*>(outData));
		break;
	case Type::HdrColor:
		ReadTypedValueList<HdrColor,HdrColor::value_type>(*static_cast<HdrColor*>(outData));
		break;
	case Type::Vector2i:
		ReadTypedValueList<Vector2i,Vector2i::value_type>(*static_cast<Vector2i*>(outData));
		break;
	case Type::Vector3i:
		ReadTypedValueList<Vector3i,Vector3i::value_type>(*static_cast<Vector3i*>(outData));
		break;
	case Type::Vector4i:
		ReadTypedValueList<Vector4i,Vector4i::value_type>(*static_cast<Vector4i*>(outData));
		break;
	case Type::Blob:
	{
		auto &blob = *static_cast<Blob*>(outData);
		ReadBlobData(blob.data);
		break;
	}
	case Type::BlobLz4:
	{
		auto &blob = *static_cast<BlobLz4*>(outData);
		blob.uncompressedSize = 0;
		ReadValueList(type,[this,&blob]() -> bool {
			static_assert(sizeof(blob.uncompressedSize) == size_of(Type::UInt64));
			ReadValue(udm::Type::UInt64,&blob.uncompressedSize);
			return true;
		});
		
		ReadBlobData(blob.compressedData);
		break;
	}
	case Type::Element:
	{
		auto t = ReadNextToken();
		if(t != '{')
			throw BuildException<SyntaxError>("Expected '{' for array element block definition, got '" +std::string{t} +"'");
		if(ReadBlockKeyValues(*static_cast<Element*>(outData)) == BlockResult::EndOfFile)
			throw BuildException<SyntaxError>("Unexpected end of file");
		break;
	}
	case Type::Array:
	case Type::ArrayLz4:
	{
		auto t = ReadNextToken();
		if(t != '[')
			throw BuildException<SyntaxError>("Expected '[' to initiate value list, got '" +std::string{t} +"'");
		auto valueType = Type::Invalid;
		auto sValueType = ReadString();
		valueType = ascii_type_to_enum(sValueType);
		if(valueType == Type::Invalid)
			throw BuildException<SyntaxError>("Invalid value type '" +sValueType +"' specified for array!");
		
		auto &a = *static_cast<Array*>(outData);
		a.SetValueType(valueType);

		if(valueType == Type::Struct)
		{
			auto *strct = a.GetStructuredDataInfo();
			if(!strct)
				throw ImplementationError{"Invalid array structure info!"};
			auto &types = strct->types;
			auto &names = strct->names;
			ReadTemplateParameterList(types,names);
		}

		t = ReadNextToken();
		std::optional<uint32_t> size {};
		if(t == ';')
		{
			size = uint32_t{};
			ReadValue(Type::UInt32,&*size);
			t = ReadNextToken();
		}

		if(t != ']')
			throw BuildException<SyntaxError>("Expected ']' to close value list, got '" +std::string{t} +"'");

		if(type == Type::ArrayLz4)
		{
			if(size.has_value() == false)
				throw BuildException<SyntaxError>("Missing size for compressed array");

			auto &a = *static_cast<ArrayLz4*>(outData);
			a.InitializeSize(*size);
			auto &blob = a.GetCompressedBlob();
			ReadBlobData(blob.compressedData);
			break;
		}
		
		if(!size.has_value())
			*size = 10;
		auto szValue = a.GetValueSize();
		a.Resize(*size);

		uint32_t numValues = 0;
		auto *p = static_cast<uint8_t*>(a.GetValues());
		std::function<void(uint32_t)> fReadValue = nullptr;
		if(valueType == Type::Struct)
		{
			auto &strct = *a.GetStructuredDataInfo();
			fReadValue = [this,&strct,&p,szValue](uint32_t idx) {
				ReadStructValue(strct,p);
				p += szValue;
			};
		}
		else
		{
			fReadValue = [this,&p,valueType,szValue](uint32_t idx) {
				ReadValue(valueType,p);
				p += szValue;
			};
		}

		ReadValueList(valueType,[this,&fReadValue,&a,&numValues]() -> bool {
			if(numValues >= a.GetSize())
				a.Resize(numValues *2 +20);
			fReadValue(numValues);
			++numValues;
			return true;
		},false);
		a.Resize(numValues);
		break;
	}
	case Type::Half:
		*static_cast<Half*>(outData) = util::to_float(ReadString());
		break;
	case Type::Struct:
	{
		auto &strct = *static_cast<Struct*>(outData);
		auto &types = strct->types;
		auto &names = strct->names;
		strct.data.resize(strct->GetDataSizeRequirement());
		ReadStructValue(*strct,strct.data.data());
		break;
	}
	};
	static_assert(umath::to_integral(Type::Count) == 36,"Update this list when new types are added!");
}

udm::AsciiReader::BlockResult udm::AsciiReader::ReadBlockKeyValues(Element &parent)
{
	auto &f = m_file;
	for(;;)
	{
		auto t = ReadNextToken();
		if(t == '$')
		{
			// Variable
			auto type = ReadString();
			auto eType = ascii_type_to_enum(type.c_str());
			if(eType == udm::Type::Invalid)
				throw BuildException<SyntaxError>("Invalid keyvalue type '" +type +"' found");
			auto prop = Property::Create(eType);
			if(eType == Type::Struct)
			{
				auto &strct = prop->GetValue<Struct>();
				auto &types = strct->types;
				auto &names = strct->names;
				ReadTemplateParameterList(types,names);
			}
			auto key = ReadString(ReadNextToken());
			SeekNextToken();
			ReadValue(eType,prop->value);
			parent.AddChild(key,prop);
			continue;
		}
		if(t == '}')
			return BlockResult::EndOfBlock;
		if(t == EOF)
			return BlockResult::EndOfFile;
		if(is_control_character(t))
			throw BuildException<SyntaxError>("Expected variable or child block, got unexpected control character '" +std::string{t} +"'");
		// Name of a new block
		auto childBlockName = ReadString(t);
		t = ReadNextToken();
		if(t != '{')
			throw BuildException<SyntaxError>("Expected '{' for child block definition, got '" +std::string{t} +"'");
		
		auto child = Property::Create<Element>();
		if(ReadBlockKeyValues(child->GetValue<Element>()) == BlockResult::EndOfFile)
			throw BuildException<SyntaxError>("Unexpected end of file");
		parent.AddChild(childBlockName,child);
	}
	// Unreachable
	return BlockResult::EndOfFile;
}

bool udm::Data::SaveAscii(const std::string &fileName,bool includeHeader) const
{
	auto f = FileManager::OpenFile<VFilePtrReal>(fileName.c_str(),"w");
	if(f == nullptr)
	{
		throw FileError{"Unable to open file!"};
		return false;
	}
	return SaveAscii(f,includeHeader);
}
bool udm::Data::SaveAscii(VFilePtrReal &f,bool includeHeader) const
{
	std::stringstream ss;
	ToAscii(ss,includeHeader);
	f->WriteString(ss.str());
	return true;
}

std::shared_ptr<udm::Data> udm::AsciiReader::LoadAscii(const VFilePtr &f)
{
	auto udmData = std::shared_ptr<udm::Data>{new udm::Data{}};
	auto rootProp = Property::Create<Element>();
	AsciiReader reader {};
	reader.m_file = f;
	auto res = reader.ReadBlockKeyValues(rootProp->GetValue<Element>());
	if(res != BlockResult::EndOfFile)
		throw reader.BuildException<SyntaxError>("Block has been terminated improperly");
	
	auto assetData = udmData->GetAssetData();
	auto udmAssetData = (*rootProp)[Data::KEY_ASSET_DATA];
	if(!udmAssetData)
	{
		auto assetDataProp = rootProp;
		rootProp = Property::Create<Element>();
		auto &elRoot = rootProp->GetValue<Element>();
		elRoot.AddChild(Data::KEY_ASSET_DATA,assetDataProp);
		elRoot[Data::KEY_ASSET_VERSION] = static_cast<uint32_t>(1);
		elRoot[Data::KEY_ASSET_TYPE] = "nil";
	}
	udmData->m_rootProperty = rootProp;
	return udmData->ValidateHeaderProperties() ? udmData : nullptr;
}

namespace udm
{
	std::shared_ptr<udm::Data> load_ascii(const VFilePtr &f)
	{
		return udm::AsciiReader::LoadAscii(f);
	}
};

char udm::AsciiReader::ReadChar()
{
	auto c = m_file->ReadChar();
	if(c == EOF)
		return EOF;
	if(c == '\n')
	{
		++m_curLine;
		m_curCharPos = 0;
	}
	else
		++m_curCharPos;
	return c;
}

char udm::AsciiReader::PeekNextChar()
{
	auto &f = m_file;
	auto pos = f->Tell();
	auto c = f->ReadChar();
	f->Seek(pos);
	return c;
}

char udm::AsciiReader::ReadUntil(char c,std::string *optOut)
{
	auto &f = m_file;
	auto cur = ReadChar();
	for(;;)
	{
		if(cur == EOF || cur == c)
			return cur;
		if(optOut)
			*optOut += cur;
		cur = ReadChar();
	}
	if(optOut)
		*optOut += cur;
	return cur;
}

void udm::AsciiReader::SeekNextToken(const std::optional<char> &tSeek)
{
	auto t = ReadNextToken();
	if(t == EOF)
		return;
	if(tSeek.has_value() && t != *tSeek)
		return SeekNextToken(tSeek);
	assert(m_curCharPos > 0);
	--m_curCharPos;
	m_file->Seek(m_file->Tell() -1);
}

void udm::AsciiReader::MoveCursorForward(uint32_t n)
{
	for(auto i=decltype(n){0u};i<n;++i)
		ReadChar();
}

char udm::AsciiReader::ReadNextToken()
{
	auto &f = m_file;
	int c;
	for(;;)
	{
		c = ReadChar();
		if(c == EOF)
			return EOF;
		if(ustring::WHITESPACE.find(c) != std::string::npos)
			continue;
		if(c == '/')
		{
			auto cNext = PeekNextChar();
			if(cNext == '/')
			{
				f->Seek(f->Tell() +1);
				ReadUntil('\n');
				continue;
			}
			else if(cNext == '*')
			{
				f->Seek(f->Tell() +1);
				for(;;)
				{
					if(ReadUntil('*') == EOF)
						return EOF;
					if(PeekNextChar() == '/')
					{
						f->Seek(f->Tell() +1);
						break;
					}
				}
				continue;
			}
		}
		return c;
	}
	return EOF;
}

std::string udm::AsciiReader::ReadString(char initialC)
{
	auto &f = m_file;
	std::string str;
	auto t = initialC;
	if(t == EOF)
		return str;
	if(is_control_character(initialC))
		throw BuildException<SyntaxError>("Expected string, got control character '" +std::string{t} +"'");
	if(t == '\"')
	{
		for(;;)
		{
			auto c = ReadUntil('\"',&str);
			if(c == EOF)
				throw BuildException<SyntaxError>("Expected quotation mark to end string, got EOF");
			if(str.size() <= 1 || str.back() != '\\')
				return str;
			continue;
		}
		// Unreachable
		return str;
	}

	f->Seek(f->Tell() -1);
	--m_curCharPos;
	for(;;)
	{
		t = PeekNextChar();
		if(t == EOF)
			return str;
		if(is_whitespace_character(t) || is_control_character(t))
			return str;
		str += ReadChar();
	}
	return str;
}

std::string udm::AsciiReader::ReadString()
{
	auto &f = m_file;
	return ReadString(f->ReadChar());
}

template<typename T,typename TBase>
	void udm::AsciiReader::ReadTypedValueList(T &outData)
{
	static_assert((sizeof(T) %sizeof(TBase)) == 0);
	constexpr auto numExpectedValues = sizeof(T) /sizeof(TBase);
	std::array<TBase,numExpectedValues> values;
	uint32_t idx = 0;
	constexpr auto type = udm::type_to_enum<TBase>();
	ReadValueList(type,[this,&type,&values,&idx]() -> bool {
		if(idx >= values.size())
			return false;
		ReadValue(type,&values[idx++]);
		return true;
	});
	if(idx != values.size())
		throw BuildException<SyntaxError>("Expected " +std::to_string(values.size()) +" values for property definition, got " +std::to_string(idx) +" at");
	memcpy(&outData,values.data(),sizeof(outData));
}

std::string udm::Property::ToAsciiValue(const Nil &nil,const std::string &prefix) {return "";}
std::string udm::Property::ToAsciiValue(const Blob &blob,const std::string &prefix)
{
	try
	{
		return "[" +util::base64_encode(blob.data.data(),blob.data.size()) +"]";
	}
	catch(const std::runtime_error &e)
	{
		throw CompressionError{e.what()};
	}
	return "";
}
std::string udm::Property::ToAsciiValue(const BlobLz4 &blob,const std::string &prefix)
{
	try
	{
		return "[" +std::to_string(blob.uncompressedSize) +"][" +util::base64_encode(blob.compressedData.data(),blob.compressedData.size()) +"]";
	}
	catch(const std::runtime_error &e)
	{
		throw CompressionError{e.what()};
	}
	return "";
}
std::string udm::Property::ToAsciiValue(const Utf8String &utf8,const std::string &prefix)
{
	try
	{
		return "[base64][" +util::base64_encode(utf8.data.data(),utf8.data.size()) +']';
	}
	catch(const std::runtime_error &e)
	{
		throw CompressionError{e.what()};
	}
	return "";
}
std::string udm::Property::ToAsciiValue(const Element &el,const std::string &prefix)
{
	// Unreachable
	throw std::runtime_error{"Cannot convert value of type Element to ASCII!"};
}
std::string udm::Property::ToAsciiValue(const Array &a,const std::string &prefix)
{
	auto valueType = a.GetValueType();
	std::stringstream ss;
	ss<<"[";
	ss<<enum_type_to_ascii(valueType);
	if(valueType == Type::Struct)
		ss<<a.GetStructuredDataInfo()->GetTemplateArgumentList();
	ss<<';'<<a.GetSize();
	ss<<"][";

	if(valueType == Type::Element)
	{
		auto *ptr = static_cast<const Element*>(a.GetValues());
		for(auto i=decltype(a.GetSize()){0u};i<a.GetSize();++i)
		{
			if(i > 0)
				ss<<',';
			ss<<"\n";
			ss<<prefix<<"\t{\n";
			ptr->ToAscii(ss,prefix +"\t");
			ss<<"\n"<<prefix<<"\t}";
			++ptr;
		}
		if(a.GetSize() > 0)
			ss<<"\n"<<prefix;
	}
	else if(valueType == Type::Struct)
	{
		auto *ptr = static_cast<const uint8_t*>(a.GetValues());
		auto &strctDesc = *a.GetStructuredDataInfo();
		auto sz = strctDesc.GetDataSizeRequirement();
		auto insertNewLine = true;

		// Note: We want some nice, clear formatting for the structured data. Putting everything into a separate line would
		// bloat the file, so we'll put multiple items into the same line until we reach maxLenPerLine characters, then we
		// use the number of items we've written so far as a reference for when to put the next new-lines (to ensure that
		// each line has the same number of items).
		uint32_t curLen = 0;
		constexpr uint32_t maxLenPerLine = 160;
		std::optional<uint32_t> nPerLine {};
		auto n = a.GetSize();
		auto subPrefix = prefix +'\t';
		for(auto i=decltype(n){0u};i<n;++i)
		{
			if(i > 0)
				ss<<',';
			if(insertNewLine)
			{
				ss<<"\n"<<subPrefix;
				curLen = 0;
				insertNewLine = false;
			}
			else
				ss<<' ';
			auto l = ss.tellp();
			ss<<StructToAsciiValue(strctDesc,ptr);
			curLen += ss.tellp() -l;
			if(nPerLine.has_value())
			{
				if(((i -1) %*nPerLine) == 0)
					insertNewLine = true;
			}
			else if(curLen > maxLenPerLine)
			{
				insertNewLine = true;
				nPerLine = i +1;
			}

			ptr += sz;
		}
		if(n > 0)
			ss<<'\n'<<prefix;
	}
	else if(is_numeric_type(valueType))
	{
		auto tag = get_numeric_tag(valueType);
		std::visit([&a,&ss](auto tag) {
			using T = decltype(tag)::type;
			auto *ptr = static_cast<const T*>(a.GetValues());
			for(auto i=decltype(a.GetSize()){0u};i<a.GetSize();++i)
			{
				if(i > 0)
					ss<<",";
				NumericTypeToString(*ptr,ss);
				++ptr;
			}
		},tag);
	}
	else
	{
		auto vs = [&a,&ss,&prefix](auto tag){
			using T = decltype(tag)::type;
			auto *ptr = static_cast<const T*>(a.GetValues());
			for(auto i=decltype(a.GetSize()){0u};i<a.GetSize();++i)
			{
				if(i > 0)
					ss<<",";
				ss<<ToAsciiValue(*ptr,prefix);
				++ptr;
			}
		};
		if(is_generic_type(valueType))
			std::visit(vs,get_generic_tag(valueType));
		else if(is_non_trivial_type(valueType))
			std::visit(vs,get_non_trivial_tag(valueType));
	}
	ss<<"]";
	return ss.str();
}
std::string udm::Property::ToAsciiValue(const ArrayLz4 &a,const std::string &prefix)
{
	auto valueType = a.GetValueType();
	try
	{
		auto &blob = a.GetCompressedBlob();
		auto stype = std::string{enum_type_to_ascii(valueType)};
		if(valueType == Type::Struct)
			stype += a.GetStructuredDataInfo()->GetTemplateArgumentList();
		return "[" +stype +';' +std::to_string(a.GetSize()) +"][" +util::base64_encode(blob.compressedData.data(),blob.compressedData.size()) +"]";
	}
	catch(const std::runtime_error &e)
	{
		throw CompressionError{e.what()};
	}
	return "";
}
std::string udm::Property::ToAsciiValue(const String &str,const std::string &prefix)
{
	auto val = str;
	ustring::replace(val,"\\","\\\\");
	return '\"' +val +'\"';
}
std::string udm::Property::ToAsciiValue(const Reference &ref,const std::string &prefix)
{
	return ToAsciiValue(ref.path,prefix);
}
std::string udm::Property::StructToAsciiValue(const StructDescription &strct,const void *data,const std::string &prefix)
{
	std::stringstream ss;
	ss<<prefix<<'[';
	auto n = strct.GetMemberCount();
	auto *ptr = static_cast<const uint8_t*>(data);
	for(auto i=decltype(n){0u};i<n;++i)
	{
		if(i > 0)
			ss<<',';
		auto type = strct.types[i];
		if(is_numeric_type(type))
		{
			std::visit([ptr,&ss](auto tag) {
				using T = decltype(tag)::type;
				NumericTypeToString(*reinterpret_cast<const T*>(ptr),ss);
			},get_numeric_tag(type));
		}
		else if(is_generic_type(type))
		{
			std::visit([ptr,&ss](auto tag) {
				using T = decltype(tag)::type;
				ss<<ToAsciiValue(*reinterpret_cast<const T*>(ptr));
			},get_generic_tag(type));
		}
		else
			throw InvalidUsageError{"Non-trivial types are not allowed for structs!"};
		ptr += size_of(type);
	}
	ss<<']';
	return ss.str();
}
std::string udm::Property::ToAsciiValue(const Struct &strct,const std::string &prefix) {return StructToAsciiValue(*strct,strct.data.data(),prefix);}
		
std::string udm::Property::ToAsciiValue(const Vector2 &v,const std::string &prefix) {return '[' +NumericTypeToString(v.x) +',' +NumericTypeToString(v.y) +']';}
std::string udm::Property::ToAsciiValue(const Vector2i &v,const std::string &prefix) {return '[' +NumericTypeToString(v.x) +',' +NumericTypeToString(v.y) +']';}
std::string udm::Property::ToAsciiValue(const Vector3 &v,const std::string &prefix) {return '[' +NumericTypeToString(v.x) +',' +NumericTypeToString(v.y) +',' +NumericTypeToString(v.z) +']';}
std::string udm::Property::ToAsciiValue(const Vector3i &v,const std::string &prefix) {return '[' +NumericTypeToString(v.x) +',' +NumericTypeToString(v.y) +',' +NumericTypeToString(v.z) +']';}
std::string udm::Property::ToAsciiValue(const Vector4 &v,const std::string &prefix) {return '[' +NumericTypeToString(v.x) +',' +NumericTypeToString(v.y) +',' +NumericTypeToString(v.z) +',' +NumericTypeToString(v.w) +']';}
std::string udm::Property::ToAsciiValue(const Vector4i &v,const std::string &prefix) {return '[' +NumericTypeToString(v.x) +',' +NumericTypeToString(v.y) +',' +NumericTypeToString(v.z) +',' +NumericTypeToString(v.w) +']';}
std::string udm::Property::ToAsciiValue(const Quaternion &q,const std::string &prefix) {return '[' +NumericTypeToString(q.w) +',' +NumericTypeToString(q.x) +',' +NumericTypeToString(q.y) +',' +NumericTypeToString(q.z) +']';}
std::string udm::Property::ToAsciiValue(const EulerAngles &a,const std::string &prefix) {return '[' +NumericTypeToString(a.p) +',' +NumericTypeToString(a.y) +',' +NumericTypeToString(a.r) +']';}
std::string udm::Property::ToAsciiValue(const Srgba &srgb,const std::string &prefix) {return '[' +NumericTypeToString(srgb[0]) +',' +NumericTypeToString(srgb[1]) +',' +NumericTypeToString(srgb[2]) +',' +NumericTypeToString(srgb[3]) +']';}
std::string udm::Property::ToAsciiValue(const HdrColor &col,const std::string &prefix) {return '[' +NumericTypeToString(col[0]) +',' +NumericTypeToString(col[1]) +',' +NumericTypeToString(col[2]) +']';}
std::string udm::Property::ToAsciiValue(const Transform &t,const std::string &prefix)
{
	auto &pos = t.GetOrigin();
	auto &rot = t.GetRotation();
	std::string s = "[";
	s += "[" +NumericTypeToString(pos.x) +',' +NumericTypeToString(pos.y) +',' +NumericTypeToString(pos.z) +"]";
	s += "[" +NumericTypeToString(rot.w) +',' +NumericTypeToString(rot.x) +',' +NumericTypeToString(rot.y) +',' +NumericTypeToString(rot.z) +"]";
	s += "]";
	return s;
}
std::string udm::Property::ToAsciiValue(const ScaledTransform &t,const std::string &prefix)
{
	auto &pos = t.GetOrigin();
	auto &rot = t.GetRotation();
	auto &scale = t.GetScale();
	std::string s = "[";
	s += "[" +NumericTypeToString(pos.x) +',' +NumericTypeToString(pos.y) +',' +NumericTypeToString(pos.z) +"]";
	s += "[" +NumericTypeToString(rot.w) +',' +NumericTypeToString(rot.x) +',' +NumericTypeToString(rot.y) +',' +NumericTypeToString(rot.z) +"]";
	s += "[" +NumericTypeToString(scale.x) +',' +NumericTypeToString(scale.y) +',' +NumericTypeToString(scale.z) +"]";
	s += "]";
	return s;
}
std::string udm::Property::ToAsciiValue(const Mat4 &m,const std::string &prefix)
{
	std::string s {"["};
	for(uint8_t i=0;i<4;++i)
	{
		s += '[';
		for(uint8_t j=0;j<4;++j)
		{
			if(j > 0)
				s += ',';
			s += NumericTypeToString(m[i][j]);
		}
		s += ']';
	}
	s += "]";
	return s;
}
std::string udm::Property::ToAsciiValue(const Mat3x4 &m,const std::string &prefix)
{
	std::string s {"["};
	for(uint8_t i=0;i<3;++i)
	{
		s += '[';
		for(uint8_t j=0;j<4;++j)
		{
			if(j > 0)
				s += ',';
			s += NumericTypeToString(m[i][j]);
		}
		s += ']';
	}
	s += "]";
	return s;
}
#pragma optimize("",on)