#include <sharedutils/util_library.hpp>
#include <sharedutils/util_path.hpp>
#include <sharedutils/util.h>
#include <fsys/filesystem.h>

#pragma optimize("",off)
int main(int argc,char *argv[])
{
	auto programPath = util::Path::CreatePath(util::get_program_path());
	programPath.PopBack(); // Go up from "bin" directory
	util::set_program_path(programPath.GetString());
	util::set_current_working_directory(programPath.GetString());

	auto path = util::Path::CreatePath(FileManager::GetRootPath());
	path += "modules/unirender";

	auto libPath = util::Path::CreatePath(FileManager::GetProgramPath());
	libPath += "bin/render_raytracing_lib.dll";

	auto fileRootPath = util::Path::CreatePath(FileManager::GetRootPath());
	FileManager::SetAbsoluteRootPath(fileRootPath.GetString());

	FileManager::AddCustomMountDirectory("materials");
	std::vector<std::string> addons {};
	FileManager::FindFiles("addons/*",nullptr,&addons);
	for(auto &addon : addons)
		FileManager::AddCustomMountDirectory(("addons/" +addon).c_str());

	std::string err;
	auto lib = util::Library::Load(libPath.GetString(),{path.GetString()},&err);
	if(lib == nullptr)
	{
		std::cout<<"Unable to load library '"<<libPath.GetString()<<"': "<<err<<std::endl;
		return EXIT_FAILURE;
	}
	auto *f = lib->FindSymbolAddress<int(*)(int,char*[])>("render_raytracing");
	if(f == nullptr)
	{
		std::cout<<"Unable to locate symbol address for 'render_raytracing' in library '"<<libPath.GetString()<<"'!"<<std::endl;
		return EXIT_FAILURE;
	}
	auto result = f(argc,argv);

	// TODO: We'll force an exit, since doing a clean exit causes it to permanently freeze
	// Fix this issue!
	exit(EXIT_SUCCESS);
	lib = nullptr;
	return result;
}
#pragma optimize("",on)
