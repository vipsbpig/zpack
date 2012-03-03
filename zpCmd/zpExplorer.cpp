#include "Stdafx.h"
#include "zpExplorer.h"
#include "zpack.h"
#include <cassert>
#include <fstream>
#include <algorithm>
#include "fileEnum.h"
#include "windows.h"
//#include "PerfUtil.h"

using namespace std;

///////////////////////////////////////////////////////////////////////////////////////////////////
ZpExplorer::ZpExplorer()
	: m_pack(NULL)
	, m_callback(NULL)
	, m_fileCount(0)
	, m_callbackParam(NULL)
{
	m_root.isDirectory = true;
	m_root.parent = NULL;
	m_currentNode = &m_root;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
ZpExplorer::~ZpExplorer()
{
	if (m_pack != NULL)
	{
		zp::close(m_pack);
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void ZpExplorer::setCallback(zp::Callback callback, void* param)
{
	m_callback = callback;
	m_callbackParam = param;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool ZpExplorer::open(const zp::String& path, bool readonly)
{
	//BEGIN_PERF("open")
	clear();
	m_pack = zp::open(path.c_str(), readonly ? zp::FLAG_READONLY : 0);
	if (m_pack == NULL)
	{
		return false;
	}
	build();
	//END_PERF
	return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool ZpExplorer::create(const zp::String& path, const zp::String& inputPath)
{
	clear();
	if (path.empty())
	{
		return false;
	}
	m_pack = zp::create(path.c_str());
	if (m_pack == NULL)
	{
		return false;
	}
	if (inputPath.empty())
	{
		return true;
	}
	m_basePath = inputPath;
	if (m_basePath.c_str()[m_basePath.length() - 1] != DIR_CHAR)
	{
		m_basePath += DIR_STR;
	}
	enumFile(m_basePath, addPackFile, this);
	return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void ZpExplorer::close()
{
	clear();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool ZpExplorer::isOpen() const
{
	return (m_pack != NULL);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool ZpExplorer::defrag()
{
	if (m_pack == NULL)
	{
		return false;
	}
	return m_pack->defrag(m_callback, m_callbackParam);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
zp::IPackage* ZpExplorer::getPack() const
{
	return m_pack;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
const zp::Char* ZpExplorer::packageFilename() const
{
	if (m_pack != NULL)
	{
		return m_pack->packageFilename();
	}
	return _T("");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
const zp::String& ZpExplorer::currentPath() const
{
	return m_currentPath;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool ZpExplorer::enterDir(const zp::String& path)
{
	assert(m_currentNode != NULL);
#if !(ZP_CASE_SENSITIVE)
	zp::String lowerPath = path;
	transform(path.begin(), path.end(), lowerPath.begin(), ::tolower);
	ZpNode* child = findChildRecursively(m_currentNode, lowerPath, FIND_DIR);
#else
	ZpNode* child = findChildRecursively(m_currentNode, path, FIND_DIR);
#endif
	if (child == NULL)
	{
		return false;
	}
	m_currentNode = child;
	getNodePath(m_currentNode, m_currentPath);
	return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool ZpExplorer::add(const zp::String& srcPath, const zp::String& dstPath)
{
	if (m_pack == NULL || m_pack->readonly() || srcPath.empty())
	{
		return false;
	}
	if (dstPath.empty())
	{
		m_workingPath = m_currentPath;
	}
	else if (dstPath[0] == DIR_CHAR)
	{
		m_workingPath = dstPath.substr(1, dstPath.length() - 1);
	}
	else
	{
		m_workingPath = m_currentPath + dstPath;
	}
	if (!m_workingPath.empty() && m_workingPath[m_workingPath.length() - 1] != DIR_CHAR)
	{
		m_workingPath += DIR_STR;
	}
	m_basePath.clear();
	size_t pos = srcPath.rfind(DIR_CHAR);

	WIN32_FIND_DATA fd;
	HANDLE findFile = ::FindFirstFile(srcPath.c_str(), &fd);
	if (findFile != INVALID_HANDLE_VALUE && (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
	{
		//it's a file
		zp::String nakedFilename = srcPath.substr(pos + 1, srcPath.length() - pos - 1);
		bool ret = addFile(srcPath, nakedFilename);
		m_pack->flush();
		::FindClose(findFile);
		return ret;
	}
	//it's a directory
	if (pos != zp::String::npos)
	{
		//dir
		m_basePath = srcPath.substr(0, pos + 1);
	}
	zp::String searchDirectory = srcPath;
	if (srcPath.c_str()[srcPath.length() - 1] != DIR_CHAR)
	{
		searchDirectory += DIR_STR;
	}
	enumFile(searchDirectory, addPackFile, this);
	m_pack->flush();
	::FindClose(findFile);
	return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool ZpExplorer::remove(const zp::String& path)
{
	if (m_pack == NULL || m_pack->readonly() || path.empty())
	{
		return false;
	}
	list<ZpNode>::iterator found;
#if !(ZP_CASE_SENSITIVE)
	zp::String lowerPath = path;
	transform(path.begin(), path.end(), lowerPath.begin(), ::tolower);
	ZpNode* child = findChildRecursively(m_currentNode, lowerPath, FIND_ANY);
#else
	ZpNode* child = findChildRecursively(m_currentNode, path, FIND_ANY);
#endif
	if (child == NULL)
	{
		return false;
	}
	zp::String internalPath;
	getNodePath(child, internalPath);
	//remove '\'
	if (!internalPath.empty())
	{
		internalPath.resize(internalPath.size() - 1);
	}
	bool ret = false;
	if (removeChildRecursively(child, internalPath))
	{
		ret = true;
		if (child->parent != NULL)
		{
			if (child == m_currentNode)
			{
				m_currentNode = child->parent;
			}
			removeChild(child->parent, child);
		}
	}
	getNodePath(m_currentNode, m_currentPath);
	m_pack->flush();
	return ret;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool ZpExplorer::extract(const zp::String& srcPath, const zp::String& dstPath)
{
	if (m_pack == NULL)
	{
		return false;
	}
	zp::String externalPath = dstPath;
	if (externalPath.empty())
	{
		externalPath = _T(".")DIR_STR;
	}
	else if (externalPath.c_str()[externalPath.length() - 1] != DIR_CHAR)
	{
		externalPath += DIR_STR;
	}
#if !(ZP_CASE_SENSITIVE)
	zp::String lowerPath = srcPath;
	transform(srcPath.begin(), srcPath.end(), lowerPath.begin(), ::tolower);
	ZpNode* child = findChildRecursively(m_currentNode, lowerPath, FIND_ANY);
#else
	ZpNode* child = findChildRecursively(m_currentNode, srcPath, FIND_ANY);
#endif
	if (child == NULL)
	{
		return false;
	}
	zp::String internalPath;
	getNodePath(child, internalPath);
	//remove '\'
	if (!internalPath.empty())
	{
		internalPath.resize(internalPath.size() - 1);
	}
	return extractRecursively(child, externalPath, internalPath);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void ZpExplorer::setCurrentNode(const ZpNode* node)
{
	assert(node != NULL);
	m_currentNode = const_cast<ZpNode*>(node);
	getNodePath(m_currentNode, m_currentPath);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
const ZpNode* ZpExplorer::currentNode() const
{
	return m_currentNode;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
const ZpNode* ZpExplorer::rootNode() const
{
	return &m_root;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void ZpExplorer::clear()
{
	m_root.children.clear();
	m_root.userData = NULL;
	m_currentNode = &m_root;
	getNodePath(m_currentNode, m_currentPath);
	m_workingPath = m_currentPath;
	if (m_pack != NULL)
	{
		zp::close(m_pack);
		m_pack = NULL;
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void ZpExplorer::build()
{
	unsigned long count = m_pack->getFileCount();
	for (unsigned long i = 0; i < count; ++i)
	{
		zp::Char buffer[256];
		zp::u32 fileSize;
		m_pack->getFileInfo(i, buffer, sizeof(buffer)/sizeof(zp::Char), &fileSize);
		zp::String filename = buffer;
		insertFileToTree(filename, fileSize, false);
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool ZpExplorer::addFile(const zp::String& filename, const zp::String& relativePath)
{
	if (m_callback != NULL && !m_callback(relativePath.c_str(), m_callbackParam))
	{
		return false;
	}
	zp::u32 fileSize = 0;
	fstream stream;
	locale loc = locale::global(locale(""));
	stream.open(filename.c_str(), ios_base::in | ios_base::binary);
	locale::global(loc);
	if (!stream.is_open())
	{
		return false;
	}
	stream.seekg(0, ios::end);
	fileSize = static_cast<zp::u32>(stream.tellg());
	char* buffer = new char[fileSize];
	stream.seekg(0, ios::beg);
	stream.read(buffer, fileSize);
	stream.close();

	zp::String internalName = m_workingPath + relativePath;
	bool succeeded = m_pack->addFile(internalName.c_str(), buffer, fileSize, zp::FLAG_REPLACE);
	delete[] buffer;
	if (!succeeded)
	{
		return false;
	}
	insertFileToTree(internalName, fileSize, true);
	return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool ZpExplorer::extractFile(const zp::String& externalPath, const zp::String& internalPath)
{
	assert(m_pack != NULL);
	zp::IFile* file = m_pack->openFile(internalPath.c_str());
	if (file == NULL)
	{
		return false;
	}
	fstream stream;
	locale loc = locale::global(locale(""));
	stream.open(externalPath.c_str(), ios_base::out | ios_base::trunc | ios_base::binary);
	locale::global(loc);
	if (!stream.is_open())
	{
		return false;
	}
	char* buffer = new char[file->size()];
	file->read(buffer, file->size());
	stream.write(buffer, file->size());
	stream.close();
	m_pack->closeFile(file);
	delete [] buffer;
	return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void ZpExplorer::countChildRecursively(const ZpNode* node)
{
	if (!node->isDirectory)
	{
		++m_fileCount;
	}
	for (list<ZpNode>::const_iterator iter = node->children.begin();
		iter != node->children.end();
		++iter)
	{
		countChildRecursively(&(*iter));
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool ZpExplorer::removeChild(ZpNode* node, ZpNode* child)
{
	assert(node != NULL && child != NULL);
	for (list<ZpNode>::iterator iter = node->children.begin();
		iter != node->children.end();
		++iter)
	{
		if (child == &(*iter))
		{
			node->children.erase(iter);
			return true;
		}
	}
	return false;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool ZpExplorer::removeChildRecursively(ZpNode* node, zp::String path)
{
	assert(node != NULL && m_pack != NULL);
	if (!node->isDirectory)
	{
		if (m_callback != NULL && !m_callback(node->name.c_str(), m_callbackParam))
		{
			return false;
		}
		if (!m_pack->removeFile(path.c_str()))
		{
			return false;
		}
		return true;
	}
	if (!path.empty())
	{
		path += DIR_STR;
	}
	//recurse
	for (list<ZpNode>::iterator iter = node->children.begin();
		iter != node->children.end();)
	{
		ZpNode* child = &(*iter);
		if (!removeChildRecursively(child, path + child->name))
		{
			return false;
		}
		if (child == m_currentNode)
		{
			m_currentNode = m_currentNode->parent;
			assert(m_currentNode != NULL);
		}
		iter = node->children.erase(iter);
	}
	return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool ZpExplorer::extractRecursively(ZpNode* node, zp::String externalPath, zp::String internalPath)
{
	assert(node != NULL && m_pack != NULL);
	externalPath += node->name;
	if (!node->isDirectory)
	{
		if (m_callback != NULL && !m_callback(internalPath.c_str(), m_callbackParam))
		{
			return false;
		}
		if (!extractFile(externalPath, internalPath))
		{
			return false;
		}
		return true;
	}
	if (!internalPath.empty())	//in case extract the root directory
	{
		externalPath += DIR_STR;
		internalPath += DIR_STR;
		//create directory if necessary
		WIN32_FIND_DATA fd;
		HANDLE findFile = ::FindFirstFile(externalPath.c_str(), &fd);
		if (findFile == INVALID_HANDLE_VALUE)
		{
			if (!::CreateDirectory(externalPath.c_str(), NULL))
			{
				return false;
			}
		}
		else if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
		{
			return false;
		}
	}
	//recurse
	for (list<ZpNode>::iterator iter = node->children.begin();
		iter != node->children.end();
		++iter)
	{
		if (!extractRecursively(&(*iter), externalPath, internalPath + iter->name))
		{
			return false;
		}
	}
	return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void ZpExplorer::insertFileToTree(const zp::String& filename, unsigned long fileSize, bool checkFileExist)
{
	ZpNode* node = &m_root;
	zp::String filenameLeft = filename;
	while (filenameLeft.length() > 0)
	{
		size_t pos = filenameLeft.find_first_of(DIR_STR);
		if (pos == zp::String::npos)
		{
			//it's a file
		#if !(ZP_CASE_SENSITIVE)
			zp::String lowerName = filenameLeft;
			transform(filenameLeft.begin(), filenameLeft.end(), lowerName.begin(), ::tolower);
			ZpNode* child = checkFileExist ? findChild(node, lowerName, FIND_FILE) : NULL;
		#else
			ZpNode* child = checkFileExist ? findChild(node, filenameLeft, FIND_FILE) : NULL;
		#endif
			if (child == NULL)
			{
				ZpNode newNode;
				newNode.parent = node;
				newNode.isDirectory = false;
				newNode.name = filenameLeft;
			#if !(ZP_CASE_SENSITIVE)
				newNode.lowerName = lowerName;
			#endif
				newNode.fileSize = fileSize;
				node->children.push_back(newNode);
			}
			else
			{
				child->fileSize = fileSize;
			}
			return;
		}
		zp::String dirName = filenameLeft.substr(0, pos);
		filenameLeft = filenameLeft.substr(pos + 1, filenameLeft.length() - pos - 1);
	#if !(ZP_CASE_SENSITIVE)
		zp::String lowerName = dirName;
		transform(dirName.begin(), dirName.end(), lowerName.begin(), ::tolower);
		ZpNode* child = findChild(node, lowerName, FIND_DIR);
	#else
		ZpNode* child = findChild(node, dirName, FIND_DIR);
	#endif
		if (child != NULL)
		{
			node = child;
		}
		else
		{
			ZpNode newNode;
			newNode.isDirectory = true;
			newNode.parent = node;
			newNode.name = dirName;
		#if !(ZP_CASE_SENSITIVE)
			newNode.lowerName = lowerName;
		#endif
			newNode.fileSize = 0;
			//insert after all directories
			list<ZpNode>::iterator insertPoint;
			for (insertPoint = node->children.begin(); insertPoint != node->children.end();	++insertPoint)
			{
				if (!insertPoint->isDirectory)
				{
					break;
				}
			}
			list<ZpNode>::iterator inserted;
			inserted = node->children.insert(insertPoint, newNode);
			node = &(*inserted);
		}
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////////
ZpNode* ZpExplorer::findChild(ZpNode* node, const zp::String& name, FindType type)
{
	if (name.empty())
	{
		return  type == FIND_FILE ? NULL : &m_root;
	}
	assert(node != NULL);
	if (name == _T("."))
	{
		return type == FIND_FILE ? NULL : node;
	}
	else if (name == _T(".."))
	{
		return type == FIND_FILE ? NULL : node->parent;
	}
	for (list<ZpNode>::iterator iter = node->children.begin();
		iter != node->children.end();
		++iter)
	{
		if ((type == FIND_DIR && !iter->isDirectory) || (type == FIND_FILE && iter->isDirectory))
		{
			continue;
		}
	#if !(ZP_CASE_SENSITIVE)
		if (name == iter->lowerName)
		{
			return &(*iter);
		}
	#else
		if (name == iter->name)
		{
			return &(*iter);
		}
	#endif
	}
	return NULL;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
ZpNode* ZpExplorer::findChildRecursively(ZpNode* node, const zp::String& name, FindType type)
{
	size_t pos = name.find_first_of(DIR_STR);
	if (pos == zp::String::npos)
	{
		//doesn't have any directory name
		return name.empty()? node : findChild(node, name, type);
	}
	ZpNode* nextNode = NULL;
	zp::String dirName = name.substr(0, pos);
	nextNode = findChild(node, dirName, FIND_DIR);
	if (nextNode == NULL)
	{
		return NULL;
	}
	zp::String nameLeft = name.substr(pos + 1, name.length() - pos - 1);
	return findChildRecursively(nextNode, nameLeft, type);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void ZpExplorer::getNodePath(const ZpNode* node, zp::String& path) const
{
	path.clear();
	while (node != NULL && node != &m_root)
	{
		path = node->name + DIR_STR + path;
		node = node->parent;
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////////
unsigned long ZpExplorer::countDiskFile(const zp::String& path)
{
	m_basePath = path;
	if (m_basePath.c_str()[m_basePath.length() - 1] != DIR_CHAR)
	{
		m_basePath += DIR_STR;
	}
	m_fileCount = 0;
	enumFile(m_basePath, countFile, this);
	if (m_fileCount == 0)
	{
		return 1;	//single file
	}
	return m_fileCount;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
unsigned long ZpExplorer::countNodeFile(const ZpNode* node)
{
	m_fileCount = 0;
	countChildRecursively(node);
	return m_fileCount;
}