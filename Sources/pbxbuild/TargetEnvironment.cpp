// Copyright 2013-present Facebook. All Rights Reserved.

#include <pbxbuild/TargetEnvironment.h>
#include <pbxbuild/BuildContext.h>

using pbxbuild::TargetEnvironment;
using pbxbuild::BuildEnvironment;
using pbxbuild::BuildContext;
using libutil::FSUtil;

TargetEnvironment::
TargetEnvironment()
{
}

TargetEnvironment::
~TargetEnvironment()
{
}

static std::unordered_map<pbxproj::PBX::BuildFile::shared_ptr, std::string>
BuildFileDisambiguation(pbxproj::PBX::Target::shared_ptr const &target)
{
    std::unordered_map<std::string, pbxproj::PBX::BuildFile::shared_ptr> buildFileUnambiguous;
    std::unordered_map<pbxproj::PBX::BuildFile::shared_ptr, std::string> buildFileDisambiguation;

    for (pbxproj::PBX::BuildPhase::shared_ptr const &buildPhase : target->buildPhases()) {
        for (pbxproj::PBX::BuildFile::shared_ptr const &buildFile : buildPhase->files()) {
            std::string name;
            if (buildFile->fileReference() != nullptr) {
                name = buildFile->fileReference()->name();
            } else if (buildFile->referenceProxy() != nullptr) {
                name = buildFile->referenceProxy()->name();
            } else {
                continue;
            }
            name = FSUtil::GetBaseNameWithoutExtension(name);

            auto it = buildFileUnambiguous.find(name);
            if (it != buildFileUnambiguous.end()) {
                buildFileDisambiguation.insert({ it->second, it->first + "-" + it->second->blueprintIdentifier() });
                buildFileDisambiguation.insert({ buildFile, name + "-" + buildFile->blueprintIdentifier() });
            }
            buildFileUnambiguous.insert({ name, buildFile });
        }
    }

    return buildFileDisambiguation;
}

static pbxproj::XC::BuildConfiguration::shared_ptr
ConfigurationNamed(pbxproj::XC::ConfigurationList::shared_ptr const &configurationList, std::string const &configuration)
{
    if (configurationList == nullptr) {
        return nullptr;
    }

    auto configurationIterator = std::find_if(configurationList->begin(), configurationList->end(), [&](pbxproj::XC::BuildConfiguration::shared_ptr buildConfiguration) -> bool {
        return buildConfiguration->name() == configuration;
    });

    if (configurationIterator == configurationList->end()) {
        return nullptr;
    }

    return *configurationIterator;
}

static pbxsetting::XC::Config::shared_ptr
LoadConfigurationFile(pbxproj::XC::BuildConfiguration::shared_ptr const &buildConfiguration, pbxsetting::Environment const &environment)
{
    if (buildConfiguration->baseConfigurationReference() == nullptr) {
        return nullptr;
    }

    pbxsetting::Value configurationValue = buildConfiguration->baseConfigurationReference()->resolve();
    std::string configurationPath = environment.expand(configurationValue);
    return pbxsetting::XC::Config::Open(configurationPath, environment);
}

static pbxsetting::Level
PlatformArchitecturesLevel(pbxspec::Manager::shared_ptr const &specManager, std::string const &specDomain)
{
    std::vector<pbxsetting::Setting> architectureSettings;
    std::vector<std::string> platformArchitectures;

    pbxspec::PBX::Architecture::vector architectures = specManager->architectures(specDomain);
    for (pbxspec::PBX::Architecture::shared_ptr const &architecture : architectures) {
        if (!architecture->architectureSetting().empty()) {
            architectureSettings.push_back(architecture->defaultSetting());
        }
        if (architecture->realArchitectures().empty()) {
            if (std::find(platformArchitectures.begin(), platformArchitectures.end(), architecture->identifier()) == platformArchitectures.end()) {
                platformArchitectures.push_back(architecture->identifier());
            }
        }
    }

    std::string platformArchitecturesValue;
    for (std::string const &arch : platformArchitectures) {
        if (&arch != &platformArchitectures[0]) {
            platformArchitecturesValue += " ";
        }
        platformArchitecturesValue += arch;
    }
    architectureSettings.push_back(pbxsetting::Setting::Parse("VALID_ARCHS", platformArchitecturesValue));

    return pbxsetting::Level(architectureSettings);
}

static xcsdk::SDK::Target::shared_ptr
FindPlatformTarget(std::shared_ptr<xcsdk::SDK::Manager> const &sdkManager, std::string const &sdkroot)
{
    for (xcsdk::SDK::Platform::shared_ptr const &platform : sdkManager->platforms()) {
        for (xcsdk::SDK::Target::shared_ptr const &sdk : platform->targets()) {
            if (sdk->canonicalName() == sdkroot || sdk->path() == sdkroot) {
                return sdk;
            }
        }

        if (platform->name() == sdkroot || platform->path() == sdkroot) {
            return platform->targets().back();
        }
    }

    return nullptr;
}

static pbxsetting::Level
PackageTypeLevel(pbxspec::PBX::PackageType::shared_ptr const &packageType)
{
    std::vector<pbxsetting::Setting> settings = {
        pbxsetting::Setting::Parse("PACKAGE_TYPE", packageType->identifier()),
    };

    pbxsetting::Level packageTypeLevel = packageType->defaultBuildSettings();
    settings.insert(settings.end(), packageTypeLevel.settings().begin(), packageTypeLevel.settings().end());

    return pbxsetting::Level(settings);
}


static pbxsetting::Level
ProductTypeLevel(pbxspec::PBX::ProductType::shared_ptr const &productType)
{
    std::vector<pbxsetting::Setting> settings = {
        pbxsetting::Setting::Parse("PRODUCT_TYPE", productType->identifier()),
    };

    pbxsetting::Level productTypeLevel = productType->defaultBuildProperties();
    settings.insert(settings.end(), productTypeLevel.settings().begin(), productTypeLevel.settings().end());

    return pbxsetting::Level(settings);
}

static pbxspec::PBX::BuildSystem::shared_ptr
TargetBuildSystem(pbxspec::Manager::shared_ptr const &specManager, std::string const &specDomain, pbxproj::PBX::Target::shared_ptr const &target)
{
    if (target->type() == pbxproj::PBX::Target::kTypeNative) {
        return specManager->buildSystem("com.apple.build-system.native", specDomain);
    } else if (target->type() == pbxproj::PBX::Target::kTypeLegacy) {
        return specManager->buildSystem("com.apple.build-system.jam", specDomain);
    } else if (target->type() == pbxproj::PBX::Target::kTypeAggregate) {
       return specManager->buildSystem("com.apple.build-system.external", specDomain);
    } else {
        fprintf(stderr, "error: unknown target type\n");
        return nullptr;
    }
}

static std::vector<std::string>
ResolveArchitectures(pbxsetting::Environment const &environment)
{
    std::vector<std::string> archsVector = pbxsetting::Type::ParseList(environment.resolve("ARCHS"));
    std::set<std::string> archs = std::set<std::string>(archsVector.begin(), archsVector.end());
    std::vector<std::string> validArchsVector = pbxsetting::Type::ParseList(environment.resolve("VALID_ARCHS"));
    std::set<std::string> validArchs = std::set<std::string>(validArchsVector.begin(), validArchsVector.end());

    std::vector<std::string> architectures;
    std::set_intersection(archs.begin(), archs.end(), validArchs.begin(), validArchs.end(), std::back_inserter(architectures));
    return architectures;
}

static std::vector<std::string>
ResolveVariants(pbxsetting::Environment const &environment)
{
    return pbxsetting::Type::ParseList(environment.resolve("BUILD_VARIANTS"));
}

static pbxsetting::Level
ArchitecturesVariantsLevel(std::vector<std::string> const &architectures, std::vector<std::string> const &variants)
{
    std::vector<pbxsetting::Setting> settings = {
        pbxsetting::Setting::Parse("CURRENT_VARIANT", variants.front()),
        pbxsetting::Setting::Parse("variant", variants.front()),
        pbxsetting::Setting::Parse("CURRENT_ARCH", architectures.front()),
        pbxsetting::Setting::Parse("arch", architectures.front()),
    };

    for (std::string const &variant : variants) {
        pbxsetting::Setting objectFileDir = pbxsetting::Setting::Parse("OBJECT_FILE_DIR_" + variant, "$(OBJECT_FILE_DIR)-" + variant);
        settings.push_back(objectFileDir);

        for (std::string const &arch : architectures) {
            std::string linkFileList = "LINK_FILE_LIST_" + variant + "_" + arch;
            std::string linkFileListPath = "$(OBJECT_FILE_DIR_" + variant + ")/" + arch + "/$(PRODUCT_NAME).LinkFileList";
            settings.push_back(pbxsetting::Setting::Parse(linkFileList, linkFileListPath));
        }
    }

    return pbxsetting::Level(settings);
}

std::unique_ptr<TargetEnvironment> TargetEnvironment::
Create(BuildEnvironment const &buildEnvironment, pbxproj::PBX::Target::shared_ptr const &target, BuildContext const *context)
{
    xcsdk::SDK::Target::shared_ptr sdk;
    std::string specDomain;
    pbxproj::XC::BuildConfiguration::shared_ptr projectConfiguration;
    pbxproj::XC::BuildConfiguration::shared_ptr targetConfiguration;
    pbxsetting::XC::Config::shared_ptr projectConfigurationFile;
    pbxsetting::XC::Config::shared_ptr targetConfigurationFile;
    {
        // FIXME(grp): $(SRCROOT) must be set in order to find the xcconfig, but we need the xcconfig to know $(SDKROOT). So this can't
        // use the default level order, because $(SRCROOT) comes below $(SDKROOT). Hack around this for now with a synthetic environment.
        // It's also in the wrong order because project settings should be below the SDK, but are needed to *load* the xcconfig.
        pbxsetting::Environment determinationEnvironment = buildEnvironment.baseEnvironment();
        determinationEnvironment.insertFront(context->baseSettings(), false);

        projectConfiguration = ConfigurationNamed(target->project()->buildConfigurationList(), context->configuration());
        if (projectConfiguration == nullptr) {
            fprintf(stderr, "error: unable to find project configuration %s\n", context->configuration().c_str());
            return nullptr;
        }

        determinationEnvironment.insertFront(target->project()->settings(), false);
        determinationEnvironment.insertFront(projectConfiguration->buildSettings(), false);

        pbxsetting::Environment projectActionEnvironment = determinationEnvironment;
        projectActionEnvironment.insertFront(context->actionSettings(), false);

        projectConfigurationFile = LoadConfigurationFile(projectConfiguration, projectActionEnvironment);
        if (projectConfigurationFile != nullptr) {
            determinationEnvironment.insertFront(projectConfigurationFile->level(), false);
        }

        targetConfiguration = ConfigurationNamed(target->buildConfigurationList(), context->configuration());
        if (targetConfiguration == nullptr) {
            fprintf(stderr, "error: unable to find target configuration %s\n", context->configuration().c_str());
            return nullptr;
        }

        determinationEnvironment.insertFront(target->settings(), false);
        determinationEnvironment.insertFront(targetConfiguration->buildSettings(), false);

        // FIXME(grp): Similar issue for the target xcconfig. These levels aren't complete (no platform) but are needed to *get* which SDK to use.
        pbxsetting::Environment targetActionEnvironment = determinationEnvironment;
        targetActionEnvironment.insertFront(context->actionSettings(), false);

        targetConfigurationFile = LoadConfigurationFile(targetConfiguration, targetActionEnvironment);
        if (targetConfigurationFile != nullptr) {
            determinationEnvironment.insertFront(targetConfigurationFile->level(), false);
        }

        determinationEnvironment.insertFront(context->actionSettings(), false);
        std::string sdkroot = determinationEnvironment.resolve("SDKROOT");
        sdk = FindPlatformTarget(buildEnvironment.sdkManager(), sdkroot);
        if (sdk == nullptr) {
            fprintf(stderr, "error: unable to find sdkroot %s\n", sdkroot.c_str());
            return nullptr;
        }

        specDomain = sdk->platform()->name();
        std::string platformSpecificationPath = pbxspec::Manager::DomainSpecificationRoot(sdk->platform()->path());
        buildEnvironment.specManager()->registerDomain(specDomain, platformSpecificationPath);
    }

    pbxspec::PBX::BuildSystem::shared_ptr buildSystem = TargetBuildSystem(buildEnvironment.specManager(), specDomain, target);
    if (buildSystem == nullptr) {
        fprintf(stderr, "error: unable to create build system\n");
        return nullptr;
    }

    pbxspec::PBX::ProductType::shared_ptr productType = nullptr;
    pbxspec::PBX::PackageType::shared_ptr packageType = nullptr;
    if (target->type() == pbxproj::PBX::Target::kTypeNative) {
        pbxproj::PBX::NativeTarget::shared_ptr nativeTarget = std::static_pointer_cast<pbxproj::PBX::NativeTarget>(target);

        productType = buildEnvironment.specManager()->productType(nativeTarget->productType(), specDomain);
        if (productType == nullptr) {
            fprintf(stderr, "error: unable to find product type %s\n", nativeTarget->productType().c_str());
            return nullptr;
        }

        // FIXME(grp): Should this always use the first package type?
        packageType = buildEnvironment.specManager()->packageType(productType->packageTypes().at(0), specDomain);
        if (packageType == nullptr) {
            fprintf(stderr, "error: unable to find package type %s\n", productType->packageTypes().at(0).c_str());
            return nullptr;
        }
    }

    // Now we have $(SDKROOT), and can make the real levels.
    pbxsetting::Environment environment = buildEnvironment.baseEnvironment();
    environment.insertFront(buildSystem->defaultSettings(), true);
    environment.insertFront(context->baseSettings(), false);
    environment.insertFront(pbxsetting::Level({
        pbxsetting::Setting::Parse("GCC_VERSION", "$(DEFAULT_COMPILER)"),
    }), false);

    environment.insertFront(sdk->platform()->defaultProperties(), false);
    environment.insertFront(PlatformArchitecturesLevel(buildEnvironment.specManager(), specDomain), false);
    environment.insertFront(sdk->defaultProperties(), false);
    environment.insertFront(sdk->platform()->settings(), false);
    environment.insertFront(sdk->settings(), false);
    environment.insertFront(sdk->customProperties(), false);
    environment.insertFront(sdk->platform()->overrideProperties(), false);

    environment.insertFront(target->project()->settings(), false);
    if (projectConfigurationFile != nullptr) {
        environment.insertFront(projectConfigurationFile->level(), false);
    }
    environment.insertFront(projectConfiguration->buildSettings(), false);

    if (packageType != nullptr) {
        environment.insertFront(PackageTypeLevel(packageType), false);
    }
    if (productType != nullptr) {
        environment.insertFront(ProductTypeLevel(productType), false);
    }

    environment.insertFront(target->settings(), false);
    environment.insertFront(targetConfiguration->buildSettings(), false);
    if (targetConfigurationFile != nullptr) {
        environment.insertFront(targetConfigurationFile->level(), false);
    }

    environment.insertFront(context->actionSettings(), false);

    std::vector<std::string> architectures = ResolveArchitectures(environment);
    std::vector<std::string> variants = ResolveVariants(environment);
    environment.insertFront(ArchitecturesVariantsLevel(architectures, variants), false);

    environment.insertFront(pbxsetting::Level({
        pbxsetting::Setting::Parse("SDKROOT", sdk->path()),
    }), false);

    auto buildRules = std::make_shared <pbxbuild::TargetBuildRules> (pbxbuild::TargetBuildRules::Create(buildEnvironment.specManager(), specDomain, target));
    auto buildFileDisambiguation = BuildFileDisambiguation(target);
    std::string workingDirectory = target->project()->basePath();

    std::unique_ptr<TargetEnvironment> te = std::make_unique<TargetEnvironment>();
    te->_buildRules = buildRules;
    te->_environment = std::make_unique<pbxsetting::Environment>(environment);
    te->_variants = variants;
    te->_architectures = architectures;
    te->_buildSystem = buildSystem;
    te->_packageType = packageType;
    te->_productType = productType;
    te->_sdk = sdk;
    te->_specDomain = specDomain;
    te->_workingDirectory = workingDirectory;
    te->_buildFileDisambiguation = buildFileDisambiguation;
    return te;
}
