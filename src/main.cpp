#include <sharedutils/util_library.hpp>
#include <sharedutils/util_path.hpp>
#include <fsys/filesystem.h>

#pragma optimize("",off)
int main(int argc,char *argv[])
{
	auto path = util::Path::CreatePath(FileManager::GetRootPath());
	path.PopBack();
	path += "modules/cycles";

	auto libPath = util::Path::CreatePath(FileManager::GetProgramPath());
	libPath += "render_raytracing_lib.dll";

	auto fileRootPath = util::Path::CreatePath(FileManager::GetRootPath());
	fileRootPath.PopBack();
	FileManager::SetAbsoluteRootPath(fileRootPath.GetString());

	FileManager::AddCustomMountDirectory("materials");
	std::vector<std::string> addons {};
	FileManager::FindFiles("addons/*",nullptr,&addons);
	for(auto &addon : addons)
		FileManager::AddCustomMountDirectory(("addons/" +addon).c_str());

	std::string err;
	auto lib = util::Library::Load(libPath.GetString(),{path.GetString()},&err);
	if(lib == nullptr)
		return EXIT_FAILURE;
	auto *f = lib->FindSymbolAddress<int(*)(int,char*[])>("render_raytracing");
	if(f == nullptr)
		return EXIT_FAILURE;
	auto result = f(argc,argv);

	lib = nullptr;
	return result;
}
#pragma optimize("",on)
