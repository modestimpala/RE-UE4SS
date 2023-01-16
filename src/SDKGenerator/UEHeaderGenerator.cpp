#define NOMINMAX
#include <Windows.h>
#ifdef TEXT
#undef TEXT
#endif

#include <set>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <format>
#include <fstream>

#include <SDKGenerator/UEHeaderGenerator.hpp>
#pragma warning(disable: 4005)
#include <DynamicOutput/DynamicOutput.hpp>
#include <UE4SSProgram.hpp>
#include <Unreal/FScriptArray.hpp>
#include <Unreal/FString.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UEnum.hpp>
#include <Unreal/UInterface.hpp>
#include <Unreal/UActorComponent.hpp>
#include <Unreal/UnrealFlags.hpp>
#include <Unreal/UScriptStruct.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/FText.hpp>
#include <Unreal/AActor.hpp>
#include <Unreal/UPackage.hpp>
#include <Unreal/Property/NumericPropertyTypes.hpp>
#include <Unreal/Property/FBoolProperty.hpp>
#include <Unreal/Property/FObjectProperty.hpp>
#include <Unreal/Property/FWeakObjectProperty.hpp>
#include <Unreal/Property/FLazyObjectProperty.hpp>
#include <Unreal/Property/FSoftObjectProperty.hpp>
#include <Unreal/Property/FSoftClassProperty.hpp>
#include <Unreal/Property/FClassProperty.hpp>
#include <Unreal/Property/FStructProperty.hpp>
#include <Unreal/Property/FDelegateProperty.hpp>
#include <Unreal/Property/FMulticastInlineDelegateProperty.hpp>
#include <Unreal/Property/FMulticastSparseDelegateProperty.hpp>
#include <Unreal/Property/FEnumProperty.hpp>
#include <Unreal/Property/FArrayProperty.hpp>
#include <Unreal/Property/FMapProperty.hpp>
#include <Unreal/Property/FInterfaceProperty.hpp>
#include <Unreal/Property/FFieldPathProperty.hpp>
#include <Unreal/Property/FSetProperty.hpp>
#pragma warning(default: 4005)

namespace RC::UEGenerator
{
    using namespace RC::Unreal;

    std::map<File::StringType, UniqueName> UEHeaderGenerator::m_used_file_names{};
    std::map<UObject*, int32_t> UEHeaderGenerator::m_dependency_object_to_unique_id{};

    auto static is_subtype_struct_valid(UScriptStruct* subtype) -> bool
    {
        static FName float_interval_name = FName(STR("FloatInterval"), FNAME_Add);
        static FName spline_curves_name = FName(STR("SplineCurves"), FNAME_Add);
        static FName int32_interval_name = FName(STR("Int32Interval"), FNAME_Add);
        static FName bone_reference_name = FName(STR("BoneReference"), FNAME_Add);
        static FName overlap_result_name = FName(STR("OverlapResult"), FNAME_Add);
        static FName rich_curve_name = FName(STR("RichCurve"), FNAME_Add);

        auto subtype_name = subtype->GetNamePrivate();
        if (subtype_name == float_interval_name ||
            subtype_name == spline_curves_name ||
            subtype_name == int32_interval_name ||
            subtype_name == bone_reference_name ||
            subtype_name == overlap_result_name||
            subtype_name == rich_curve_name)
        {
            return false;
        }
        else
        {
            return true;
        }
    };

    auto static is_subtype_valid(FProperty* subtype) -> bool
    {
        if ((subtype->IsA<FNumericProperty>() && !subtype->IsA<FIntProperty>() && !subtype->IsA<FFloatProperty>() && !subtype->IsA<FByteProperty>()) ||
            subtype->IsA<FWeakObjectProperty>())
        {
            return false;
        }
        else if (auto* as_array = CastField<FArrayProperty>(subtype); as_array)
        {
            return is_subtype_valid(as_array->GetInner());
        }
        else if (auto* as_map = CastField<FMapProperty>(subtype); as_map)
        {
            return is_subtype_valid(as_map->GetKeyProp()) && is_subtype_valid(as_map->GetValueProp());
        }
        else if (auto* as_struct = CastField<FStructProperty>(subtype); as_struct)
        {
            return is_subtype_struct_valid(as_struct->GetStruct());
        }
        else if (auto* as_enum = CastField<FEnumProperty>(subtype); as_enum)
        {
            return as_enum->GetUnderlyingProperty()->IsA<FByteProperty>();
        }
        else
        {
            return true;
        }
    };

    auto string_to_uppercase(std::wstring s) -> std::wstring
    {
        std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) {
            return towupper(c);
        });
        return s;
    }

    class FlagFormatHelper
    {
        std::set<std::wstring> m_switches;
        std::map<std::wstring, std::set<std::wstring>> m_parameters;
        std::shared_ptr<FlagFormatHelper> m_meta_helper;

        FlagFormatHelper(bool is_root_helper)
        {
            if (is_root_helper)
            {
                m_meta_helper = std::shared_ptr<FlagFormatHelper>(new FlagFormatHelper(false));
            }
        }
    public:
        FlagFormatHelper() : FlagFormatHelper(true)
        {
        }

        auto add_switch(const std::wstring& switch_name) -> void
        {
            m_switches.insert(switch_name);
        }

        auto add_parameter(const std::wstring& parameter_name, const std::wstring& parameter_value) -> void
        {
            if (parameter_name == STR("meta"))
            {
                throw std::invalid_argument("Use get_meta() to add metadata to the flag declaration");
            }

            auto map_iterator = m_parameters.find(parameter_name);
            if (map_iterator != m_parameters.end())
            {
                map_iterator->second.insert(parameter_name);
            }
            else
            {
                m_parameters.insert({parameter_name, {parameter_value}});
            }
        }

        auto get_meta() const -> FlagFormatHelper*
        {
            return m_meta_helper.get();
        }

        auto build_flag_string() const -> std::wstring
        {
            std::wstring resulting_string;

            for (const std::wstring& switch_name : m_switches)
            {
                resulting_string.append(switch_name);
                resulting_string.append(STR(", "));
            }

            for (const std::pair<std::wstring, std::set<std::wstring>>& parameter_pair : m_parameters)
            {
                resulting_string.append(parameter_pair.first);
                resulting_string.append(STR("="));
                const std::set<std::wstring>& parameter_values = parameter_pair.second;

                if (parameter_values.size() != 1)
                {
                    resulting_string.append(STR("("));

                    for (const std::wstring& parameter_value : parameter_values)
                    {
                        resulting_string.append(parameter_value);
                        resulting_string.append(STR(", "));
                    }

                    if (parameter_values.size() != 0)
                    {
                        resulting_string.erase(resulting_string.size() - 1, 1);
                    }
                    resulting_string.append(STR(")"));
                }
                else
                {
                    resulting_string.append(*parameter_values.begin());
                }
                resulting_string.append(STR(", "));
            }

            if (m_meta_helper)
            {
                const std::wstring meta_flag_string = m_meta_helper->build_flag_string();
                if (!meta_flag_string.empty())
                {
                    resulting_string.append(STR("meta=("));
                    resulting_string.append(meta_flag_string);
                    resulting_string.append(STR(")"));
                    resulting_string.append(STR(", "));
                }
            }

            if (!resulting_string.empty())
            {
                resulting_string.erase(resulting_string.size() - 2, 2);
            }
            return resulting_string;
        }
    };

    auto UEHeaderGenerator::generate_module_build_file(const std::wstring& module_name) -> void
    {
        const FFilePath module_file_path = m_root_directory / module_name / fmt::format(STR("{}.Build.cs"), module_name);
        GeneratedFile module_build_file = GeneratedFile(module_file_path);

        module_build_file.append_line(STR("using UnrealBuildTool;"));
        module_build_file.append_line(STR(""));

        module_build_file.append_line(fmt::format(STR("public class {} : ModuleRules {{"), module_name));
        module_build_file.begin_indent_level();

        module_build_file.append_line(fmt::format(STR("public {}(ReadOnlyTargetRules Target) : base(Target) {{"), module_name));
        module_build_file.begin_indent_level();

        module_build_file.append_line(STR("PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;"));

        if (Version::IsAtLeast(4, 24))
        {
            module_build_file.append_line(STR("bLegacyPublicIncludePaths = false;"));
            module_build_file.append_line(STR("ShadowVariableWarningLevel = WarningLevel.Warning;"));
        }

        module_build_file.append_line(STR(""));
        module_build_file.append_line(STR("PublicDependencyModuleNames.AddRange(new string[] {"));
        module_build_file.begin_indent_level();

        std::set<std::wstring> all_module_dependencies = this->m_forced_module_dependencies;
        add_module_and_sub_module_dependencies(all_module_dependencies, module_name, false);

        for (const std::wstring& other_module_name : all_module_dependencies)
        {
            module_build_file.append_line(fmt::format(STR("\"{}\","), other_module_name));
        }

        module_build_file.end_ident_level();
        module_build_file.append_line(STR("});"));

        module_build_file.end_ident_level();
        module_build_file.append_line(STR("}"));

        module_build_file.end_ident_level();
        module_build_file.append_line(STR("}"));

        module_build_file.serialize_file_content_to_disk();
    }

    auto UEHeaderGenerator::generate_module_implementation_file(const std::wstring& module_name) -> void
    {
        const FFilePath module_file_path = m_root_directory / module_name / STR("Private") / fmt::format(STR("{}Module.cpp"), module_name);
        GeneratedFile module_impl_file = GeneratedFile(module_file_path);

        module_impl_file.append_line(STR("#include \"Modules/ModuleManager.h\""));
        module_impl_file.append_line(STR(""));
        if (module_name != m_primary_module_name) {
            module_impl_file.append_line(fmt::format(STR("IMPLEMENT_MODULE(FDefaultGameModuleImpl, {});"), module_name));
        } else {
            module_impl_file.append_line(fmt::format(STR("IMPLEMENT_PRIMARY_GAME_MODULE(FDefaultGameModuleImpl, {}, {});"), module_name, module_name));
        }

        module_impl_file.serialize_file_content_to_disk();
    }

    auto UEHeaderGenerator::generate_interface_definition(UClass* uclass, GeneratedSourceFile& header_data) -> void
    {
        const std::wstring interface_class_native_name = get_native_class_name(uclass);
        const std::wstring interface_flags_string = generate_interface_flags(uclass);

        std::wstring maybe_api_name;
        if ((uclass->GetClassFlags() & CLASS_RequiredAPI) != 0)
        {
            maybe_api_name.append(convert_module_name_to_api_name(header_data.get_header_module_name()));
            maybe_api_name.append(STR(" "));
        }

        UClass* super_class = uclass->GetSuperClass();
        header_data.add_dependency_object(super_class, DependencyLevel::Include);

        std::wstring parent_interface_class_name = get_native_class_name(super_class);

        //Generate interface UCLASS declaration
        header_data.append_line(fmt::format(STR("UINTERFACE({})"), interface_flags_string));
        header_data.append_line(fmt::format(STR("class {}{} : public {} {{"), maybe_api_name, interface_class_native_name, parent_interface_class_name));

        header_data.begin_indent_level();
        header_data.append_line(STR("GENERATED_BODY()"));
        header_data.end_ident_level();

        header_data.append_line(STR("};"));
        header_data.append_line(STR(""));

        //Generate interface real class declaration
        const std::wstring interface_native_name = get_native_class_name(uclass, true);
        const std::wstring parent_interface_name = get_native_class_name(super_class, true);

        header_data.append_line(fmt::format(STR("class {}{} : public {} {{"), maybe_api_name, interface_native_name, parent_interface_name));
        header_data.begin_indent_level();

        header_data.append_line(STR("GENERATED_BODY()"));

        AccessModifier current_access_modifier = AccessModifier::None;
        append_access_modifier(header_data, AccessModifier::Public, current_access_modifier);

        //Generate delegate type declarations for the current class
        int32_t NumDelegatesGenerated = 0;
        uclass->ForEachFunction([&](UFunction* function) {
            if (is_delegate_signature_function(function))
            {
                generate_delegate_type_declaration(function, header_data);
                NumDelegatesGenerated++;
            }
            return RC::LoopAction::Continue;
        });
        if (NumDelegatesGenerated) {
            header_data.append_line(STR(""));
        }

        //Generate interface functions
        uclass->ForEachFunction([&](UFunction* function) {
            if (!is_delegate_signature_function(function))
            {
                append_access_modifier(header_data, get_function_access_modifier(function), current_access_modifier);
                generate_function(uclass, function, header_data, true, CaseInsensitiveSet{});
            }
            return RC::LoopAction::Continue;
        });

        header_data.end_ident_level();
        header_data.append_line(STR("};"));
    }

    auto UEHeaderGenerator::generate_object_definition(UClass* uclass, GeneratedSourceFile& header_data) -> void
    {
        const std::wstring class_native_name = get_native_class_name(uclass);
        const std::wstring class_flags_string = generate_class_flags(uclass);

        std::wstring maybe_api_name;
        if ((uclass->GetClassFlags() & CLASS_RequiredAPI) != 0)
        {
            maybe_api_name.append(convert_module_name_to_api_name(header_data.get_header_module_name()));
            maybe_api_name.append(STR(" "));
        }

        UClass* super_class = uclass->GetSuperClass();
        std::wstring parent_class_name;
        if (super_class)
        {
            parent_class_name = get_native_class_name(super_class);
        }
        else
        {
            parent_class_name = STR("UObjectBaseUtility");
        }

        if (super_class)
        {
            header_data.add_dependency_object(super_class, DependencyLevel::Include);
        }

        std::wstring interface_list_string;
        auto implemented_interfaces = uclass->GetInterfaces();

        implemented_interfaces.ForEach([&](const RC::Unreal::FImplementedInterface* uinterface) {
            header_data.add_dependency_object(uinterface->Class, DependencyLevel::Include);
            const std::wstring interface_name = get_native_class_name(uinterface->Class, true);

            interface_list_string.append(STR(", public "));
            interface_list_string.append(interface_name);
            return RC::LoopAction::Continue;
        });

        header_data.append_line(fmt::format(STR("UCLASS({})"), class_flags_string));
        header_data.append_line(fmt::format(STR("class {}{} : public {}{} {{"), maybe_api_name, class_native_name, parent_class_name, interface_list_string));
        header_data.begin_indent_level();

        header_data.append_line(STR("GENERATED_BODY()"));

        AccessModifier current_access_modifier = AccessModifier::None;
        append_access_modifier(header_data, AccessModifier::Public, current_access_modifier);

        CaseInsensitiveSet blacklisted_property_names = collect_blacklisted_property_names(uclass);
        bool encountered_replicated_properties = false;

        //Generate delegate type declarations for the current class
        int32_t NumDelegatesGenerated = 0;
        uclass->ForEachFunction([&](UFunction* function) {
            if (is_delegate_signature_function(function))
            {
                generate_delegate_type_declaration(function, header_data);
                NumDelegatesGenerated++;
            }
            return RC::LoopAction::Continue;
        });
        if (NumDelegatesGenerated) {
            header_data.append_line(STR(""));
        }

        //Generate properties
        uclass->ForEachProperty([&](FProperty* property) {
            encountered_replicated_properties |= (property->GetPropertyFlags() & CPF_Net) != 0;
            append_access_modifier(header_data, get_property_access_modifier(property), current_access_modifier);
            generate_property(uclass, property, header_data);
            
            if (auto as_map_property = CastField<FMapProperty>(property))
            {
                if (auto as_struct_property = CastField<FStructProperty>(as_map_property->GetKeyProp()))
                {
                    // Add GetKeyProp() to vector for second pass.
                    m_structs_that_need_get_type_hash.emplace(as_struct_property->GetStruct());
                }
            }

            return RC::LoopAction::Continue;
        });

        //Make sure constructor and replicated properties methods are public
        append_access_modifier(header_data, AccessModifier::Public, current_access_modifier);

        //Generate constructor
        header_data.append_line(fmt::format(STR("{}();"), class_native_name));

        //Generate GetLifetimeReplicatedProps override if we have encountered replicated properties
        if (encountered_replicated_properties)
        {
            header_data.append_line(STR("virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;"));
            header_data.append_line(STR(""));
        }

        //Generate functions
        std::unordered_set<FName> implemented_functions;
        uclass->ForEachFunction([&](UFunction* function) {
            if (!is_delegate_signature_function(function))
            {
                append_access_modifier(header_data, get_function_access_modifier(function), current_access_modifier);
                generate_function(uclass, function, header_data, false, blacklisted_property_names);
                implemented_functions.emplace(function->GetNamePrivate());
            }
            return RC::LoopAction::Continue;
        });

        // Generate overrides for all inherited virtual functions
        if (implemented_interfaces.Num() > 0)
        {
            header_data.append_line(STR(""));
            header_data.append_line(STR("// Fix for true pure virtual functions not being implemented"));
        }
        implemented_interfaces.ForEach([&](const RC::Unreal::FImplementedInterface* uinterface) {
            uinterface->Class->ForEachFunction([&](UFunction* interface_function) {
                bool should_skip = (interface_function->GetFunctionFlags() & FUNC_BlueprintEvent);

                if (!implemented_functions.contains(interface_function->GetNamePrivate()) && !should_skip)
                {
                    append_access_modifier(header_data, get_function_access_modifier(interface_function), current_access_modifier);
                    generate_function(uclass, interface_function, header_data, false, blacklisted_property_names, true);
                }
                return LoopAction::Continue;
            });

            return LoopAction::Continue;
        });

        header_data.end_ident_level();
        header_data.append_line(STR("};"));
    }

    auto UEHeaderGenerator::generate_struct_definition(UScriptStruct* script_struct, GeneratedSourceFile& header_data) -> void
    {
        const std::wstring struct_native_name = get_native_struct_name(script_struct);
        const std::wstring struct_flags_string = generate_struct_flags(script_struct);

        std::wstring api_macro_name = convert_module_name_to_api_name(header_data.get_header_module_name());
        api_macro_name.append(STR(" "));
        bool is_struct_exported = (script_struct->GetStructFlags() & STRUCT_RequiredAPI) != 0;

        UScriptStruct* super_struct = script_struct->GetSuperScriptStruct();
        std::wstring parent_struct_declaration;
        if (super_struct)
        {
            header_data.add_dependency_object(super_struct, DependencyLevel::Include);

            const std::wstring super_struct_native_name = get_native_struct_name(super_struct);
            parent_struct_declaration.append(fmt::format(STR(" : public {}"), super_struct_native_name));
        }

        header_data.append_line(fmt::format(STR("USTRUCT({})"), struct_flags_string));
        header_data.append_line(fmt::format(STR("struct {}{}{} {{"), is_struct_exported ? api_macro_name : STR(""), struct_native_name, parent_struct_declaration));
        header_data.begin_indent_level();

        header_data.append_line(STR("GENERATED_BODY()"));

        AccessModifier current_access_modifier = AccessModifier::None;
        append_access_modifier(header_data, AccessModifier::Public, current_access_modifier);

        //Generate struct properties
        script_struct->ForEachProperty([&](FProperty* property) {
            append_access_modifier(header_data, get_property_access_modifier(property), current_access_modifier);
            generate_property(script_struct, property, header_data);
            
            if (auto as_map_property = CastField<FMapProperty>(property))
            {
                if (auto as_struct_property = CastField<FStructProperty>(as_map_property->GetKeyProp()))
                {
                    // Add GetKeyProp() to vector for second pass.
                    m_structs_that_need_get_type_hash.emplace(as_struct_property->GetStruct());
                }
            }
            
            return RC::LoopAction::Continue;
        });

        //Generate constructor and make sure it's public
        append_access_modifier(header_data, AccessModifier::Public, current_access_modifier);
        header_data.append_line(fmt::format(STR("{}{}();"), !is_struct_exported ? api_macro_name : STR(""), struct_native_name));

        header_data.end_ident_level();
        header_data.append_line(STR("};"));
    }

    auto UEHeaderGenerator::generate_enum_definition(UEnum* uenum, GeneratedSourceFile& header_data) -> void
    {
        const std::wstring native_enum_name = get_native_enum_name(uenum, false);
        const std::wstring enum_flags_string = generate_enum_flags(uenum);

        UEnum::ECppForm cpp_form = uenum->GetCppForm();

        header_data.append_line(fmt::format(STR("UENUM({})"), enum_flags_string));

        if (cpp_form == UEnum::ECppForm::Namespaced)
        {
            header_data.append_line(fmt::format(STR("namespace {} {{"), native_enum_name));
            header_data.begin_indent_level();
            header_data.append_line(STR("enum Type {"));
        }
        else if (cpp_form == UEnum::ECppForm::Regular)
        {
            header_data.append_line(fmt::format(STR("enum {} {{"), native_enum_name));
        }
        else if (cpp_form == UEnum::ECppForm::EnumClass)
        {
            const auto underlying_type = m_underlying_enum_types.find(native_enum_name);

            if (underlying_type == m_underlying_enum_types.end())
            {
                if (UE4SSProgram::settings_manager.UHTHeaderGenerator.MakeEnumClassesBlueprintType)
                {
                    header_data.append_line(fmt::format(STR("enum class {} : uint8 {{"), native_enum_name));
                }
                else
                {
                    //Enum has never been used in any native classes or structures, go with implicit type
                    header_data.append_line(fmt::format(STR("enum class {} {{"), native_enum_name));
                }
            }
            else
            {
                std::wstring underlying_type_string = underlying_type->second;

                //If this enum is blueprint visible, it should totally be uint8 and nothing else, no matter what the underlying property says
                if (m_blueprint_visible_enums.contains(native_enum_name))
                {
                    underlying_type_string = STR("uint8");
                }
                header_data.append_line(fmt::format(STR("enum class {} : {} {{"), native_enum_name, underlying_type_string));
            }
        }

        header_data.begin_indent_level();

        int64_t expected_next_enum_value = 0;
        uenum->ForEachName([&](Unreal::FName Name, int64_t Value) {
            std::wstring result_enumeration_line = sanitize_enumeration_name(Name.ToString());
            bool enum_constant_has_explicit_number = false;

            if (Value != expected_next_enum_value)
            {
                const std::wstring MinusSign = Value < 0 ? STR("-") : STR("");
                result_enumeration_line.append(fmt::format(STR(" = {}0x{:X}"), MinusSign, std::abs(Value)));
                enum_constant_has_explicit_number = true;
            }
            expected_next_enum_value = Value + 1;

            if (result_enumeration_line.ends_with(STR("_MAX")))
            {
                const std::wstring expected_full_constant_name = fmt::format(STR("{}_MAX"), uenum->GetName());

                //Skip enum _MAX constant if it has the following number and matching name, which means it has been autogenerated
                if (result_enumeration_line == expected_full_constant_name && !enum_constant_has_explicit_number)
                {
                    return RC::LoopAction::Continue;
                }
                //Otherwise, just make sure it's hidden and not visible to the end user
                result_enumeration_line.append(STR(" UMETA(Hidden)"));
            }

            result_enumeration_line.append(STR(","));
            header_data.append_line(result_enumeration_line);
            return RC::LoopAction::Continue;
        });

        header_data.end_ident_level();
        header_data.append_line(STR("};"));

        if (cpp_form == UEnum::ECppForm::Namespaced)
        {
            header_data.end_ident_level();
            header_data.append_line(STR("}"));
        }
    }

    auto UEHeaderGenerator::generate_delegate_type_declaration(UFunction* signature_function, GeneratedSourceFile& header_data) -> void
    {
        auto function_flags = signature_function->GetFunctionFlags();
        if ((function_flags & Unreal::FUNC_Delegate) == 0) {
            throw std::runtime_error(RC::fmt("Delegate Signature function %S is missing FUNC_Delegate flag", signature_function->GetName().c_str()));
        }

        //TODO not particularly nice or reliable, but will do for now
        const bool is_sparse = signature_function->GetClassPrivate()->GetName() == STR("SparseDelegateFunction");

        const bool is_multicast = (function_flags & Unreal::FUNC_MulticastDelegate) != 0;
        const bool declared_const = (function_flags & FUNC_Const) != 0;

        const std::wstring delegate_type_name = get_native_delegate_type_name(signature_function, nullptr, true);
        FProperty* return_value_property = signature_function->GetReturnProperty();

        std::wstring delegate_macro_string;

        //Delegate macro declaration is only allowed on the top level delegates, class-based types are limited to being implicit
        if (signature_function->GetOuterPrivate()->IsA<UPackage>()) {
            delegate_macro_string.append(STR("UDELEGATE("));
            delegate_macro_string.append(generate_function_flags(signature_function));
            delegate_macro_string.append(STR(") "));
        }

        PropertyTypeDeclarationContext context(delegate_type_name, &header_data);

        int32_t num_delegate_parameters = 0;
        std::wstring delegate_parameter_list = generate_function_parameter_list(nullptr, signature_function, header_data, true, context.context_name, {}, &num_delegate_parameters);
        if (num_delegate_parameters > 0)
        {
            delegate_parameter_list.insert(0, STR(", "));
        }

        std::wstring return_value_declaration;
        if (return_value_property != NULL)
        {
            return_value_declaration = generate_property_type_declaration(return_value_property, context);
            return_value_declaration.append(STR(", "));
        }

        std::wstring delegate_declaration_string = fmt::format(STR("{}DECLARE_DYNAMIC{}{}_DELEGATE{}{}{}({}{}{});"),
                                                               delegate_macro_string,
                                                               is_multicast ? STR("_MULTICAST") : STR(""),
                                                               is_sparse ? STR("_SPARSE") : STR(""),
                                                               return_value_property ? STR("_RetVal") : STR(""),
                                                               generate_parameter_count_string(num_delegate_parameters),
                                                               declared_const ? STR("_Const") : STR(""),
                                                               return_value_declaration,
                                                               delegate_type_name,
                                                               delegate_parameter_list);

        header_data.append_line(delegate_declaration_string);
    }

    auto UEHeaderGenerator::generate_object_implementation(UClass* uclass, GeneratedSourceFile& implementation_file) -> void
    {
        const std::wstring class_native_name = get_native_class_name(uclass);
        CaseInsensitiveSet blacklisted_property_names = collect_blacklisted_property_names(uclass);

        //Generate functions
        uclass->ForEachFunction([&](UFunction* function) {
            if (!is_delegate_signature_function(function))
            {
                generate_function_implementation(uclass, function, implementation_file, false, blacklisted_property_names);
                implementation_file.append_line(STR(""));
            }
            return RC::LoopAction::Continue;
        });

        bool encountered_replicated_properties = false;

        uclass->ForEachProperty([&](FProperty* property) {
            encountered_replicated_properties |= (property->GetPropertyFlags() & CPF_Net) != 0;
            return RC::LoopAction::Continue;
        });

        //Generate replicated properties implementation if we really need it
        if (encountered_replicated_properties)
        {
            implementation_file.add_extra_include(STR("Net/UnrealNetwork.h"));

            implementation_file.append_line(fmt::format(STR("void {}::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const {{"), class_native_name));
            implementation_file.begin_indent_level();

            implementation_file.append_line(STR("Super::GetLifetimeReplicatedProps(OutLifetimeProps);"));
            implementation_file.append_line(STR(""));

            uclass->ForEachProperty([&](FProperty* property) {
                if ((property->GetPropertyFlags() & CPF_Net) != 0)
                {
                    implementation_file.append_line(fmt::format(STR("DOREPLIFETIME({}, {});"), class_native_name, property->GetName()));
                }
                return RC::LoopAction::Continue;
            });

            implementation_file.end_ident_level();
            implementation_file.append_line(STR("}"));
            implementation_file.append_line(STR(""));
        }

        std::wstring constructor_postfix_string;
        UClass* super_class = uclass->GetSuperClass();
        const std::wstring native_parent_class_name = super_class ? get_native_class_name(super_class) : STR("UObjectUtility");

        //If parent class contains the UObjectInitializer constructor without default value,
        //we need to create the explicit call to such constructor and pass UObjectInitializer::Get() as the argument
        if (m_classes_with_object_initializer.contains(native_parent_class_name))
        {
            constructor_postfix_string.append(fmt::format(STR(" : {}(FObjectInitializer::Get())"), native_parent_class_name));
        }

        //Generate constructor implementation and initialize properties inside
        implementation_file.append_line(fmt::format(STR("{}::{}(){} {{"), class_native_name, class_native_name, constructor_postfix_string));
        implementation_file.begin_indent_level();

        //Generate properties
        UObject* class_default_object = uclass->GetClassDefaultObject();

        if (class_default_object != nullptr) 
        {
            uclass->ForEachProperty([&](FProperty* property) {
                generate_property_value(property, class_default_object, implementation_file, STR("this->"));
                return RC::LoopAction::Continue;
            });
        }
        else 
        {
            implementation_file.append_line(STR("// Null default object."));
        }

        implementation_file.end_ident_level();
        implementation_file.append_line(STR("}"));
    }

    auto UEHeaderGenerator::generate_struct_implementation(UScriptStruct* script_struct, GeneratedSourceFile& implementation_file) -> void
    {
        const std::wstring struct_native_name = get_native_struct_name(script_struct);

        //Generate constructor implementation and initialize properties inside
        implementation_file.append_line(fmt::format(STR("{}::{}() {{"), struct_native_name, struct_native_name));
        implementation_file.begin_indent_level();

        //Generate properties
        void* struct_default_object = malloc(script_struct->GetPropertiesSize());

        //TODO: ScriptStruct->InitializeStruct(StructDefaultObject);
        memset(struct_default_object, 0, script_struct->GetPropertiesSize());

        script_struct->ForEachProperty([&](FProperty* property) {
            generate_property_value(property, struct_default_object, implementation_file, STR("this->"));
            return RC::LoopAction::Continue;
        });

        //TODO: ScriptStruct->DestroyStruct(StructDefaultObject);
        free(struct_default_object);

        implementation_file.end_ident_level();
        implementation_file.append_line(STR("}"));
    }

    auto UEHeaderGenerator::generate_property(UObject* uclass, FProperty* property, GeneratedSourceFile& header_data) -> void
    {
        const std::wstring property_flags_string = generate_property_flags(property);

        bool is_bitmask_bool = false;
        PropertyTypeDeclarationContext Context(uclass->GetName(), &header_data, true, &is_bitmask_bool);

        const std::wstring property_type_string = generate_property_type_declaration(property, Context);

        std::wstring property_extra_declaration;
        if (property->GetArrayDim() != 1)
        {
            property_extra_declaration.append(STR("["));
            property_extra_declaration.append(std::to_wstring(property->GetArrayDim()));
            property_extra_declaration.append(STR("]"));
        }
        else if (is_bitmask_bool)
        {
            property_extra_declaration.append(STR(": 1"));
        }

        header_data.append_line(fmt::format(STR("UPROPERTY({})"), property_flags_string));
        header_data.append_line(fmt::format(STR("{} {}{};"), property_type_string, property->GetName(), property_extra_declaration));
        header_data.append_line(STR(""));
    }

//TODO FUNC_Final is not properly handled (should be always set except some weird cases)
    auto UEHeaderGenerator::generate_function(UClass* uclass, UFunction* function, GeneratedSourceFile& header_data, bool is_generating_interface, const CaseInsensitiveSet& blacklisted_property_names, bool generate_as_override) -> void
    {
        auto function_flags = function->GetFunctionFlags();
        const std::wstring context_name = uclass->GetName();
        bool is_function_pure_virtual = generate_as_override;

        std::wstring function_modifier_string;
        if ((function_flags & FUNC_Static) != 0)
        {
            function_modifier_string.append(STR("static "));
        }
        else if ((function_flags & FUNC_BlueprintEvent) == 0 && is_generating_interface)
        {
            //When we have a blueprint function that is not blueprint event inside the interface,
            //it means we are dealing with the native interface that cannot be implemented via blueprints
            //and uses pure virtual functions implemented through native code
            function_modifier_string.append(STR("virtual "));
            is_function_pure_virtual = true;
        }

        FProperty* return_property = function->GetReturnProperty();
        std::wstring return_property_string;
        if (return_property != NULL)
        {
            PropertyTypeDeclarationContext context(uclass->GetName(), &header_data);
            return_property_string = generate_property_type_declaration(return_property, context);
        }
        else
        {
            return_property_string = STR("void");
        }

        std::wstring function_extra_postfix_string;
        if ((function_flags & FUNC_Const) != 0)
        {
            function_extra_postfix_string.append(STR(" const"));
        }
        if (is_function_pure_virtual)
        {
            std::wstring return_statement_string;
            if (return_property != NULL)
            {
                const std::wstring default_property_value = generate_default_property_value(return_property, header_data, context_name);
                return_statement_string = fmt::format(STR(" return {};"), default_property_value);
            }

            if (generate_as_override)
            {
                function_extra_postfix_string.append(STR(" override"));
            }
            function_extra_postfix_string.append(fmt::format(STR(" PURE_VIRTUAL({},{})"), function->GetName(), return_statement_string));
        }

        std::wstring function_argument_list = generate_function_parameter_list(uclass, function, header_data, false, context_name, blacklisted_property_names);

        const std::wstring function_flags_string = generate_function_flags(function, is_function_pure_virtual);
        header_data.append_line(fmt::format(STR("UFUNCTION({})"), function_flags_string));
        // Format for virtual functions
        // virtual <return type> <function_name>(<params>) PURE_VIRTUAL(<function name>, <return statement>)
        header_data.append_line(fmt::format(STR("{}{} {}({}){};"), function_modifier_string, return_property_string, function->GetName(), function_argument_list, function_extra_postfix_string));
        header_data.append_line(STR(""));
    }

    auto UEHeaderGenerator::generate_enum_value(UEnum* uenum, int64_t enum_value) -> std::wstring
    {
        UEnum::ECppForm cpp_form = uenum->GetCppForm();
        const std::wstring enum_native_name = get_native_enum_name(uenum, false);

        std::wstring enum_constant_name;
        uenum->ForEachName([&](FName Name, int64_t Value) {
            if (Value == enum_value)
            {
                enum_constant_name = sanitize_enumeration_name(Name.ToString());
            }
            return RC::LoopAction::Continue;
        });
        if (enum_constant_name.empty())
        {
            Output::send(STR("Warning: Invalid value for enum '{}', casting instead of using enum name. Value '{}' will be cast to the enum.\n"), enum_native_name, enum_value);
            return fmt::format(STR("({}){}"), enum_native_name, enum_value);
        }
        else
        {
            //Regular enumerations do not really need an enum name prefix
            if (cpp_form == UEnum::ECppForm::Regular)
            {
                return enum_constant_name;
            }
            return fmt::format(STR("{}::{}"), enum_native_name, enum_constant_name);
        }
    }

    auto UEHeaderGenerator::generate_simple_assignment_expression(FProperty* property, const std::wstring& value, GeneratedSourceFile& implementation_file, const std::wstring& property_scope) -> void
    {
        const std::wstring field_class_name = property->GetName();
        if (property->GetArrayDim() == 1)
        {
            implementation_file.append_line(fmt::format(STR("{}{} = {};"), property_scope, field_class_name, value));
        }
        else
        {
            for (int32_t i = 0; i < property->GetArrayDim(); i++)
            {
                implementation_file.append_line(fmt::format(STR("{}{}[{}] = {};"), property_scope, field_class_name, i, value));
            }
        }
    }

    auto UEHeaderGenerator::generate_property_value(FProperty* property, void* object, GeneratedSourceFile& implementation_file, const std::wstring& property_scope) -> void
    {
        const std::wstring field_class_name = property->GetClass().GetName();

        //Byte Property
        if (field_class_name == STR("ByteProperty"))
        {
            FByteProperty* byte_property = static_cast<FByteProperty*>(property);
            uint8_t* byte_property_value = byte_property->ContainerPtrToValuePtr<uint8_t>(object);

            UEnum* uenum = byte_property->GetEnum();
            std::wstring result_property_value;
            if (uenum != NULL)
            {
                result_property_value = generate_enum_value(uenum, *byte_property_value);
            }
            else
            {
                result_property_value = std::to_wstring(*byte_property_value);
            }
            generate_simple_assignment_expression(property, result_property_value, implementation_file, property_scope);
            return;
        }

        //Enum Property
        if (field_class_name == STR("EnumProperty"))
        {
            FEnumProperty* p_enum_property = static_cast<FEnumProperty*>(property);
            UEnum* uenum = p_enum_property->GetEnum();
            if (uenum == NULL)
            {
                throw std::runtime_error(RC::fmt("EnumProperty %S does not have a valid Enum value", property->GetName().c_str()));
            }

            implementation_file.add_dependency_object(uenum, DependencyLevel::Include);
            FNumericProperty* underlying_property = p_enum_property->GetUnderlyingProperty();
             int64 value = underlying_property->GetSignedIntPropertyValue(p_enum_property->ContainerPtrToValuePtr<int64>(object));
            std::wstring result_property_value = generate_enum_value(uenum, value);

            generate_simple_assignment_expression(property, result_property_value, implementation_file, property_scope);
            return;
        }

        //Bool Property
        if (field_class_name == STR("BoolProperty"))
        {
            FBoolProperty* bool_property = static_cast<FBoolProperty*>(property);
            uint8_t* RawPropertyValue = bool_property->ContainerPtrToValuePtr<uint8>(object);

            uint8_t field_mask = bool_property->GetFieldMask();
            uint8_t byte_offset = bool_property->GetByteOffset();

            uint8_t* byte_value = RawPropertyValue + byte_offset;
            bool result_bool_value = !!(*byte_value & field_mask);

            const std::wstring result_property_value = result_bool_value ? STR("true") : STR("false");
            generate_simple_assignment_expression(property, result_property_value, implementation_file, property_scope);
            return;
        }

        //Name property is generated as normal string assignment
        if (field_class_name == STR("NameProperty"))
        {
            FName* name_value = property->ContainerPtrToValuePtr<FName>(object);
            const std::wstring name_value_string = name_value->ToString();

            if (name_value_string != STR("None"))
            {
                const std::wstring result_value = fmt::format(STR("TEXT(\"{}\")"), name_value_string);
                generate_simple_assignment_expression(property, result_value, implementation_file, property_scope);
            }
            return;
        }

        //String properties are just normal strings
        if (field_class_name == STR("StrProperty"))
        {
            FString* string_value = property->ContainerPtrToValuePtr<FString>(object);
            const std::wstring string_value_string = string_value->GetCharArray();

            if (string_value_string != STR(""))
            {
                const std::wstring result_value = create_string_literal(string_value_string);
                generate_simple_assignment_expression(property, result_value, implementation_file, property_scope);
            }
            return;
        }

        //Text properties are treated as FText::FromString, although it's not ideal
        if (field_class_name == STR("TextProperty"))
        {
            FText* text_value = property->ContainerPtrToValuePtr<FText>(object);
            const std::wstring text_value_string = text_value->ToString();

            if (text_value_string != STR(""))
            {
                const std::wstring ResultValue = fmt::format(STR("FText::FromString({})"), create_string_literal(text_value_string));
                generate_simple_assignment_expression(property, ResultValue, implementation_file, property_scope);
            }
        }

        //Object Properties are handled either as NULL, default subobjects, or UClass objects
        if (field_class_name == STR("ObjectProperty"))
        {
            FObjectProperty* object_property = static_cast<FObjectProperty*>(property);
            UObject* sub_object_value = *object_property->ContainerPtrToValuePtr<UObject*>(object);

            if (sub_object_value == NULL)
            {
                //If object value is NULL, generate a simple NULL constant
                generate_simple_assignment_expression(property, STR("NULL"), implementation_file, property_scope);

            }
            else if (sub_object_value->HasAnyFlags(EObjectFlags::RF_DefaultSubObject))
            {
                //Generate a CreateDefaultSubobject function call
                UClass* object_class_type = sub_object_value->GetClassPrivate();
                implementation_file.add_dependency_object(object_class_type, DependencyLevel::Include);

                const std::wstring object_class_name = get_native_class_name(object_class_type);
                const std::wstring object_name = sub_object_value->GetName();
                const std::wstring initializer = fmt::format(STR("CreateDefaultSubobject<{}>(TEXT(\"{}\"))"), object_class_name, object_name);

                generate_simple_assignment_expression(property, initializer, implementation_file, property_scope);
            }
            else if (UClass* sub_object_as_class = Cast<UClass>(sub_object_value))
            {
                //Generate a ::StaticClass call if this object represents a class
                implementation_file.add_dependency_object(sub_object_as_class, DependencyLevel::Include);

                const std::wstring object_class_name = get_native_class_name(sub_object_as_class);
                const std::wstring initializer = fmt::format(STR("{}::StaticClass()"), object_class_name);

                generate_simple_assignment_expression(property, initializer, implementation_file, property_scope);
            }
            else
            {
                //Unhandled case, might be some external object reference
                Output::send(STR("Unhandled default value of the FObjectProperty {}: {}\n"), property->GetFullName(), sub_object_value->GetFullName());
            }
            return;
        }

        //Class Properties are handled either as NULL or StaticClass references
        if (field_class_name == STR("ClassProperty"))
        {
            FClassProperty* class_property = static_cast<FClassProperty*>(property);
            UClass* class_value = *class_property->ContainerPtrToValuePtr<UClass*>(object);

            if (class_value == NULL)
            {
                //If class value is NULL, generate a simple NULL assignment
                generate_simple_assignment_expression(property, STR("NULL"), implementation_file, property_scope);

            }
            else if ((class_value->GetClassFlags() & CLASS_Native) != 0)
            {
                implementation_file.add_dependency_object(class_value, DependencyLevel::Include);

                //Otherwise, generate StaticClass call, assuming the class is native
                const std::wstring object_class_name = get_native_class_name(class_value);
                const std::wstring initializer = fmt::format(STR("{}::StaticClass()"), object_class_name);

                generate_simple_assignment_expression(property, initializer, implementation_file, property_scope);
            }
            else
            {
                //Unhandled case, reference to the non-native blueprint class potentially?
                Output::send(STR("Unhandled default value of the FClassProperty {}: {}\n"), property->GetFullName(), class_value->GetFullName());
            }
            return;
        }

        //Struct properties are serialization as normal struct constructors with custom scope
        //TODO there are a lot of issues with that regarding member access, unnecessary assignments and so on
        /*if (FieldClassName == STR("StructProperty")) {
            XStructProperty* StructProperty = static_cast<XStructProperty*>(Property);
            UScriptStruct* ScriptStruct = StructProperty->get_script_struct();

            if (ScriptStruct == NULL) {
                throw std::runtime_error(RC::fmt("Struct is NULL for StructProperty %S", Property->GetName().c_str()));
            }

            void* StructDataPointer = StructProperty->container_ptr_to_value_ptr<void>(Object);
            const std::wstring NewPropertyScope = fmt::format(STR("{}{}."), PropertyScope, StructProperty->GetName());

            //Generate values for each struct property
            //TODO we do not really need to generate assignments for each struct member, we only really need members that are different from the constructor set defaults
            ScriptStruct->ForEachProperty([&](FProperty* NestedStructProperty) {
                GeneratePropertyValue(NestedStructProperty, StructDataPointer, ImplementationFile, NewPropertyScope);
                return RC::LoopAction::Continue;
            });
            return;
        }*/

        if (field_class_name == STR("ArrayProperty"))
        {
            FScriptArray* property_value = property->ContainerPtrToValuePtr<FScriptArray>(object);

            //Reserve the needed amount of elements
            if (property_value->Num() > 0)
            {
                implementation_file.append_line(fmt::format(STR("{}{}.AddDefaulted({});"), property_scope, property->GetName(), property_value->Num()));
            }
            return;
        }

        //TODO: Support collection/map properties initialization later
        if (field_class_name == STR("ArrayProperty"))
        {
            return;
        }
        if (field_class_name == STR("SetProperty"))
        {
            return;
        }
        if (field_class_name == STR("MapProperty"))
        {
            return;
        }

        // Used to be 'property->is_child_of'
        if (property->IsA<FNumericProperty>())
        {
            FNumericProperty* numeric_property = static_cast<FNumericProperty*>(property);

            std::wstring number_constant_string;
            if (!numeric_property->IsFloatingPoint())
            {
                int64 value = numeric_property->GetSignedIntPropertyValue(numeric_property->ContainerPtrToValuePtr<int64>(object));
                number_constant_string = std::to_wstring(value);
            }
            else
            {
                double value = numeric_property->GetFloatingPointPropertyValue(numeric_property->ContainerPtrToValuePtr<double>(object));
                number_constant_string = fmt::format(STR("{:.2f}f"), value);
            }
            generate_simple_assignment_expression(property, number_constant_string, implementation_file, property_scope);
            return;
        }
    }

    auto UEHeaderGenerator::generate_function_implementation(UClass* uclass, UFunction* function, GeneratedSourceFile& implementation_file, bool is_generating_interface, const CaseInsensitiveSet& blacklisted_property_names) -> void
    {
        const std::wstring class_native_name = get_native_class_name(uclass, is_generating_interface);
        const std::wstring raw_function_name = function->GetName();
        auto function_flags = function->GetFunctionFlags();
        PropertyTypeDeclarationContext context(uclass->GetName(), &implementation_file);

        std::wstring function_implementation_name;
        std::wstring net_validate_function_name;
        bool is_input_function_const = ((function_flags) & FUNC_Const) != 0;

        if ((function_flags & FUNC_Net) != 0)
        {
            //Network functions always have the implementation inside the _Implementation function
            function_implementation_name = fmt::format(STR("{}::{}_Implementation"), class_native_name, raw_function_name);

            //Validated network functions by default have their validation function name set to _Validate
            if ((function_flags & FUNC_NetValidate) != 0)
            {
                net_validate_function_name = fmt::format(STR("{}::{}_Validate"), class_native_name, raw_function_name);
            }
        }
        else if ((function_flags & FUNC_BlueprintEvent) != 0)
        {
            //Blueprint Events use _Implementation by default too, but only BlueprintNativeEvents.
            //BlueprintImplementableEvents do not have any native functions at all, they're just thunks
            if ((function_flags & FUNC_Native) != 0)
            {
                function_implementation_name = fmt::format(STR("{}::{}_Implementation"), class_native_name, raw_function_name);
            }
        }
        else
        {
            //Otherwise, normal UFunctions get a standard name matching the function in question
            function_implementation_name = fmt::format(STR("{}::{}"), class_native_name, raw_function_name);
        }

        std::wstring function_parameter_list;
        if (!function_implementation_name.empty() || !net_validate_function_name.empty())
        {
            function_parameter_list = generate_function_parameter_list(uclass, function, implementation_file, false, context.context_name, blacklisted_property_names);
        }

        if (!function_implementation_name.empty())
        {
            FProperty* return_value_property = function->GetReturnProperty();

            const std::wstring return_value_type = return_value_property ?
                                                   generate_property_type_declaration(return_value_property, context) : STR("void");

            implementation_file.append_line(fmt::format(STR("{} {}({}){} {{"), return_value_type, function_implementation_name,
                                                        function_parameter_list, is_input_function_const ? STR(" const") : STR("")));
            implementation_file.begin_indent_level();

            if (return_value_property != NULL)
            {
                const std::wstring default_value = generate_default_property_value(return_value_property, implementation_file, context.context_name);
                implementation_file.append_line(fmt::format(STR("return {};"), default_value));
            }

            implementation_file.end_ident_level();
            implementation_file.append_line(STR("}"));
        }

        if (!net_validate_function_name.empty())
        {
            implementation_file.append_line(fmt::format(STR("bool {}({}) {{"), net_validate_function_name, function_parameter_list));
            implementation_file.begin_indent_level();

            implementation_file.append_line(STR("return true;"));

            implementation_file.end_ident_level();
            implementation_file.append_line(STR("}"));
        }
    }

    auto UEHeaderGenerator::generate_parameter_count_string(int32_t parameter_count) -> std::wstring
    {
        switch (parameter_count)
        {
            case 0:
                return STR("");
            case 1:
                return STR("_OneParam");
            case 2:
                return STR("_TwoParams");
            case 3:
                return STR("_ThreeParams");
            case 4:
                return STR("_FourParams");
            case 5:
                return STR("_FiveParams");
            case 6:
                return STR("_SixParams");
            case 7:
                return STR("_SevenParams");
            case 8:
                return STR("_EightParams");
            case 9:
                return STR("_NineParams");
            default:
                throw std::invalid_argument("Invalid delegate parameter count");
        }
    }

    auto UEHeaderGenerator::append_access_modifier(GeneratedSourceFile& header_data, AccessModifier needed_access, AccessModifier& current_access) -> void
    {
        if (current_access != needed_access)
        {
            current_access = needed_access;

            if (needed_access == AccessModifier::Public)
            {
                header_data.append_line_no_indent(STR("public:"));
            }
            else if (needed_access == AccessModifier::Protected)
            {
                header_data.append_line_no_indent(STR("protected:"));
            }
            else if (needed_access == AccessModifier::Private)
            {
                header_data.append_line_no_indent(STR("private:"));
            }
        }
    }

    auto UEHeaderGenerator::get_function_access_modifier(UFunction* function) -> AccessModifier
    {
        auto function_flags = function->GetFunctionFlags();

        if ((function_flags & FUNC_Private) != 0)
        {
            return AccessModifier::Private;
        }
        else if ((function_flags & FUNC_Protected) != 0)
        {
            return AccessModifier::Protected;
        }
        else if ((function_flags & FUNC_Public) != 0)
        {
            return AccessModifier::Public;
        }
        return AccessModifier::Public;
    }

    auto UEHeaderGenerator::get_property_access_modifier(FProperty* property) -> AccessModifier
    {
        auto property_flags = property->GetPropertyFlags();

        if ((property_flags & CPF_NativeAccessSpecifierPublic) != 0)
        {
            return AccessModifier::Public;
        }
        else if ((property_flags & CPF_NativeAccessSpecifierProtected) != 0)
        {
            return AccessModifier::Protected;
        }
        else if ((property_flags & CPF_NativeAccessSpecifierPrivate) != 0)
        {
            return AccessModifier::Private;
        }
        return AccessModifier::Public;
    }

    auto UEHeaderGenerator::create_string_literal(const std::wstring& string) -> std::wstring
    {
        std::wstring result;
        result.append(STR("TEXT(\""));

        bool previous_character_was_hex = false;

        const wchar_t* ptr = string.c_str();
        while (wchar_t ch = *ptr++)
        {
            switch (ch)
            {
                case STR('\r'):
                {
                    continue;
                }
                case STR('\n'):
                {
                    result.append(STR("\\n"));
                    previous_character_was_hex = false;
                    break;
                }
                case STR('\\'):
                {
                    result.append(STR("\\\\"));
                    previous_character_was_hex = false;
                    break;
                }
                case STR('\"'):
                {
                    result.append(STR("\\\""));
                    previous_character_was_hex = false;
                    break;
                }
                default:
                {
                    if (ch < 31 || ch >= 128)
                    {
                        result.append(fmt::format(STR("\\x{:04X}"), ch));
                        previous_character_was_hex = true;
                    }
                    else
                    {
                        // We close and open the literal (with TEXT) here in order to ensure that successive hex characters aren't
                        // appended to the hex sequence, causing a different number
                        if (previous_character_was_hex && iswxdigit(ch) != 0)
                        {
                            result.append(STR("\")TEXT(\""));
                        }
                        previous_character_was_hex = false;
                        result.push_back(ch);
                    }
                    break;
                }
            }
        }
        result.append(STR("\")"));
        return result;
    }

    auto UEHeaderGenerator::convert_module_name_to_api_name(const std::wstring& module_name) -> std::wstring
    {
        std::wstring uppercase_string = string_to_uppercase(module_name);
        uppercase_string.append(STR("_API"));
        return uppercase_string;
    }

    auto UEHeaderGenerator::add_module_and_sub_module_dependencies(std::set<std::wstring>& out_module_dependencies, const std::wstring& module_name, bool add_self_module) -> void
    {
        // Prevent infinite recursion
        if (out_module_dependencies.contains(module_name)) { return; }

        if (add_self_module)
        {
            out_module_dependencies.insert(module_name);
        }
        const auto iterator = m_module_dependencies.find(module_name);
        if (iterator != m_module_dependencies.end())
        {
            for (const std::wstring& DependencyModuleName : *iterator->second)
            {
                add_module_and_sub_module_dependencies(out_module_dependencies, DependencyModuleName);
            }
        }
    }

    auto UEHeaderGenerator::collect_blacklisted_property_names(UObject* uclass) -> CaseInsensitiveSet
    {
        CaseInsensitiveSet result_set;

        if (uclass->GetClassPrivate()->IsChildOf(UClass::StaticClass()))
        {
            UClass* class_object = static_cast<UClass*>(uclass);

            class_object->ForEachProperty([&](FProperty* property) {
                result_set.insert(property->GetName());
                return RC::LoopAction::Continue;
            });

            class_object->ForEachFunction([&](UFunction* function) {
                result_set.insert(function->GetName());
                return RC::LoopAction::Continue;
            });
        }
        else if (uclass->GetClassPrivate()->IsChildOf(UScriptStruct::StaticClass()))
        {
            UScriptStruct* script_struct = static_cast<UScriptStruct*>(uclass);

            script_struct->ForEachProperty([&](FProperty* property) {
                result_set.insert(property->GetName());
                return RC::LoopAction::Continue;
            });
        }
        return result_set;
    }

//TODO CannotImplementInterfaceInBlueprint is not exactly right,
//TODO you can have interface with no implementable blueprint methods but that you can still implement in blueprint
    auto UEHeaderGenerator::generate_interface_flags(UClass* uinterface) const -> std::wstring
    {
        FlagFormatHelper flag_format_helper{};

        auto class_flags = uinterface->GetClassFlags();
        UClass* super_interface = uinterface->GetSuperClass();
        auto parent_class_flags = super_interface->GetClassFlags();

        auto class_own_flags = (EClassFlags)(class_flags & (~(parent_class_flags & CLASS_Inherit)));

        if ((class_own_flags & CLASS_MinimalAPI) != 0)
        {
            flag_format_helper.add_switch(STR("MinimalAPI"));
        }

        ClassBlueprintInfo blueprint_info = get_class_blueprint_info(uinterface);
        ClassBlueprintInfo parent_blueprint_info = get_class_blueprint_info(super_interface);

        if (blueprint_info.is_blueprintable)
        {
            if (!parent_blueprint_info.is_blueprintable)
            {
                flag_format_helper.add_switch(STR("Blueprintable"));
            }
        }
        else if (blueprint_info.is_blueprint_type)
        {
            if (!parent_blueprint_info.is_blueprint_type)
            {
                flag_format_helper.add_switch(STR("BlueprintType"));
            }
            flag_format_helper.get_meta()->add_switch(STR("CannotImplementInterfaceInBlueprint"));
        }

        return flag_format_helper.build_flag_string();
    }

    auto UEHeaderGenerator::generate_class_flags(UClass* uclass) const -> std::wstring
    {
        FlagFormatHelper flag_format_helper{};

        auto class_flags = uclass->GetClassFlags();
        UClass* super_class = uclass->GetSuperClass();
        auto parent_class_flags = super_class ? super_class->GetClassFlags() : EClassFlags::CLASS_None;

        auto class_own_flags = (EClassFlags)(class_flags & (~(parent_class_flags & CLASS_Inherit)));

        if ((class_own_flags & CLASS_MinimalAPI) != 0)
        {
            flag_format_helper.add_switch(STR("MinimalAPI"));
        }

        ClassBlueprintInfo blueprint_info = get_class_blueprint_info(uclass);
        ClassBlueprintInfo parent_blueprint_info{};
        if (super_class != NULL)
        {
            parent_blueprint_info = get_class_blueprint_info(super_class);
        }

        if (UE4SSProgram::settings_manager.UHTHeaderGenerator.MakeAllPropertyBlueprintsReadWrite)
        {
            flag_format_helper.add_switch(STR("Blueprintable"));
        }
        else
        {
            if (blueprint_info.is_blueprintable)
            {
                if (!parent_blueprint_info.is_blueprintable)
                {
                    flag_format_helper.add_switch(STR("Blueprintable"));
                }
            }
            else if (blueprint_info.is_blueprint_type)
            {
                if (!parent_blueprint_info.is_blueprint_type)
                {
                    flag_format_helper.add_switch(STR("BlueprintType"));
                }
            }
        }

        if ((class_own_flags & CLASS_Deprecated) != 0)
        {
            flag_format_helper.add_switch(STR("Deprecated"));
        }
        if ((class_own_flags & CLASS_Abstract) != 0)
        {
            flag_format_helper.add_switch(STR("Abstract"));
        }

        if ((class_own_flags & CLASS_MinimalAPI) != 0)
        {
            flag_format_helper.add_switch(STR("MinimalAPI"));
        }
        if ((class_own_flags & CLASS_NoExport) != 0)
        {
            flag_format_helper.add_switch(STR("NoExport"));
        }
        //TODO not quite the case, because UHT boilerplate implicitly marks every native class as CLASS_Intrinsic
        //if ((ClassOwnFlags & CLASS_Intrinsic) != 0) {
        //    FlagFormatHelper.AddSwitch(STR("Intrinsic"));
        //}

        if ((class_own_flags & CLASS_Const) != 0)
        {
            flag_format_helper.add_switch(STR("Const"));
        }
        if ((class_own_flags & CLASS_DefaultToInstanced) != 0)
        {
            flag_format_helper.add_switch(STR("DefaultToInstanced"));
        }

        UClass* class_within = uclass->GetClassWithin();
        if (class_within != NULL && class_within != UObject::StaticClass() &&
            (super_class == NULL || class_within != super_class->GetClassWithin()))
        {
            flag_format_helper.add_parameter(STR("Within"), class_within->GetName());
        }

        if ((class_own_flags & CLASS_Transient) != 0)
        {
            flag_format_helper.add_switch(STR("Transient"));
        }
        else if ((parent_class_flags & CLASS_Transient) != 0)
        {
            flag_format_helper.add_switch(STR("NonTransient"));
        }

        if ((class_own_flags & CLASS_EditInlineNew) != 0)
        {
            flag_format_helper.add_switch(STR("EditInlineNew"));
        }
        else if ((class_flags & CLASS_EditInlineNew) == 0 && (parent_class_flags & CLASS_EditInlineNew) != 0)
        {
            flag_format_helper.add_switch(STR("NotEditInlineNew"));
        }

        if ((class_own_flags & CLASS_NotPlaceable) != 0)
        {
            flag_format_helper.add_switch(STR("NotPlaceable"));
        }
        else if ((class_flags & CLASS_NotPlaceable) == 0 && (parent_class_flags & CLASS_NotPlaceable) != 0)
        {
            flag_format_helper.add_switch(STR("Placeable"));
        }

        const std::wstring class_config_name = uclass->GetClassConfigName().ToString();
        if (super_class == NULL || class_config_name != super_class->GetClassConfigName().ToString())
        {
            flag_format_helper.add_parameter(STR("Config"), class_config_name);
        }

        if ((class_own_flags & CLASS_DefaultConfig) != 0)
        {
            flag_format_helper.add_switch(STR("DefaultConfig, config = Engine"));
        }
        if ((class_own_flags & CLASS_GlobalUserConfig) != 0)
        {
            flag_format_helper.add_switch(STR("GlobalUserConfig"));
        }
        if ((class_own_flags & CLASS_ProjectUserConfig) != 0)
        {
            flag_format_helper.add_switch(STR("ProjectUserConfig, config = Engine"));
        }

        if ((class_own_flags & CLASS_PerObjectConfig) != 0)
        {
            flag_format_helper.add_switch(STR("PerObjectConfig"));
        }
        if ((class_own_flags & CLASS_ConfigDoNotCheckDefaults) != 0)
        {
            flag_format_helper.add_switch(STR("ConfigDoNotCheckDefaults"));
        }

        if ((class_own_flags & CLASS_HideDropDown) != 0)
        {
            flag_format_helper.add_switch(STR("HideDropdown"));
        }

        if ((class_own_flags & CLASS_CollapseCategories) != 0)
        {
            flag_format_helper.add_switch(STR("CollapseCategories"));
        }
        else if ((parent_class_flags & CLASS_CollapseCategories) != 0)
        {
            flag_format_helper.add_switch(STR("DontCollapseCategories"));
        }

        //Mark all UActorComponent derived classes as BlueprintSpawnableComponent by default
        //This will allow using them inside the Simple Construction Script of the blueprint assets
        if (uclass->IsChildOf<UActorComponent>())
        {
            flag_format_helper.get_meta()->add_switch(STR("BlueprintSpawnableComponent"));
            flag_format_helper.add_parameter(STR("ClassGroup"), STR("Custom"));
        }

        return flag_format_helper.build_flag_string();
    }

    /**/
    auto UEHeaderGenerator::generate_property_type_declaration(FProperty* property, const PropertyTypeDeclarationContext& context) -> std::wstring
    {
        UClass* current_class = Unreal::Cast<UClass>(property->GetOutermostOwner());
        const std::wstring field_class_name = property->GetClass().GetName();

        //Byte Property
        if (field_class_name == STR("ByteProperty"))
        {
            FByteProperty* byte_property = static_cast<FByteProperty*>(property);
            UEnum* enum_value = byte_property->GetEnum();

            if (enum_value != NULL)
            {
                if (context.source_file != NULL)
                {
                    context.source_file->add_dependency_object(enum_value, DependencyLevel::Include);
                }

                const std::wstring enum_type_name = get_native_enum_name(enum_value);

                if ((property->GetPropertyFlags() & CPF_BlueprintVisible) != 0)
                {
                    this->m_blueprint_visible_enums.insert(enum_type_name);
                }

                //Non-EnumClass enumerations should be wrapped into TEnumAsByte according to UHT
                return fmt::format(STR("TEnumAsByte<{}>"), enum_type_name);
            }
            return STR("uint8");
        }

        //Enum Property
        if (field_class_name == STR("EnumProperty"))
        {
            FEnumProperty* enum_property = static_cast<FEnumProperty*>(property);
            FNumericProperty* underlying_property = enum_property->GetUnderlyingProperty();
            UEnum* uenum = enum_property->GetEnum();

            if (uenum == NULL)
            {
                throw std::runtime_error(RC::fmt("EnumProperty %S does not have a valid Enum value", property->GetName().c_str()));
            }
            if (context.source_file != NULL)
            {
                context.source_file->add_dependency_object(uenum, DependencyLevel::Include);
            }
            const std::wstring enum_type_name = get_native_enum_name(uenum);

            if ((property->GetPropertyFlags() & CPF_BlueprintVisible) != 0)
            {
                this->m_blueprint_visible_enums.insert(enum_type_name);
            }

            const std::wstring underlying_enum_type = generate_property_type_declaration(underlying_property, context);
            this->m_underlying_enum_types.insert({enum_type_name, underlying_enum_type});
            return enum_type_name;
        }

        //Bool Property
        if (field_class_name == STR("BoolProperty"))
        {
            FBoolProperty* bool_property = static_cast<FBoolProperty*>(property);
            if (context.is_top_level_declaration && context.out_is_bitmask_bool != NULL)
            {
                if (bool_property->GetFieldMask() != 255)
                {
                    *context.out_is_bitmask_bool = true;
                    return STR("uint8");
                }
            }
            return STR("bool");
        }

        //Standard Numeric Properties
        if (field_class_name == STR("Int8Property"))
        {
            return STR("int8");
        }
        else if (field_class_name == STR("Int16Property"))
        {
            return STR("int16");
        }
        else if (field_class_name == STR("IntProperty"))
        {
            return STR("int32");
        }
        else if (field_class_name == STR("Int64Property"))
        {
            return STR("int64");
        }
        else if (field_class_name == STR("UInt16Property"))
        {
            return STR("uint16");
        }
        else if (field_class_name == STR("UInt32Property"))
        {
            return STR("uint32");
        }
        else if (field_class_name == STR("UInt64Property"))
        {
            return STR("uint64");
        }
        else if (field_class_name == STR("FloatProperty"))
        {
            return STR("float");
        }
        else if (field_class_name == STR("DoubleProperty"))
        {
            return STR("double");
        }

        //Object Properties
        // TODO: Verify that the syntax for 'AssetObjectProperty' is the same as for 'ObjectProperty'.
        //       If it's not, then add another branch here after you figure out what the syntax should be.
        if (field_class_name == STR("ObjectProperty") || field_class_name == STR("AssetObjectProperty"))
        {
            FObjectProperty* object_property = static_cast<FObjectProperty*>(property);
            UClass* property_class = object_property->GetPropertyClass();

            if (property_class == NULL)
            {
                return STR("UObject*");
            }
            if (context.source_file != NULL)
            {
                context.source_file->add_dependency_object(property_class, DependencyLevel::PreDeclaration);
            }
            const std::wstring property_class_name = get_native_class_name(property_class, false);

            return fmt::format(STR("{}*"), property_class_name);
        }

        if (auto* object_property = CastField<FObjectPtrProperty>(property); object_property)
        {
            auto* property_class = object_property->GetPropertyClass();

            if (!property_class)
            {
                return STR("TObjectPtr<UObject>");
            }
            else
            {
                if (context.source_file)
                {
                    context.source_file->add_dependency_object(property_class, DependencyLevel::PreDeclaration);
                }

                const auto property_class_name = get_native_class_name(property_class, false);
                return std::format(STR("TObjectPtr<{}>"), property_class_name);
            }
        }

        if (field_class_name == STR("WeakObjectProperty"))
        {
            FWeakObjectProperty* weak_object_property = static_cast<FWeakObjectProperty*>(property);
            UClass* property_class = weak_object_property->GetPropertyClass();

            if (property_class == NULL)
            {
                return STR("TWeakObjectPtr<UObject>");
            }

            if (context.source_file != NULL)
            {
                context.source_file->add_dependency_object(property_class, DependencyLevel::PreDeclaration);
            }
            const std::wstring property_class_name = get_native_class_name(property_class, false);

            return fmt::format(STR("TWeakObjectPtr<{}>"), property_class_name);
        }

        if (field_class_name == STR("LazyObjectProperty"))
        {
            FLazyObjectProperty* lazy_object_property = static_cast<FLazyObjectProperty*>(property);
            UClass* property_class = lazy_object_property->GetPropertyClass();

            if (property_class == NULL)
            {
                return STR("TLazyObjectPtr<UObject>");
            }

            if (context.source_file != NULL)
            {
                context.source_file->add_dependency_object(property_class, DependencyLevel::PreDeclaration);
            }
            const std::wstring property_class_name = get_native_class_name(property_class, false);

            return fmt::format(STR("TLazyObjectPtr<{}>"), property_class_name);
        }

        if (field_class_name == STR("SoftObjectProperty"))
        {
            FSoftObjectProperty* soft_object_property = static_cast<FSoftObjectProperty*>(property);
            UClass* property_class = soft_object_property->GetPropertyClass();

            if (property_class == NULL)
            {
                return STR("TSoftObjectPtr<UObject>");
            }

            if (context.source_file != NULL)
            {
                context.source_file->add_dependency_object(property_class, DependencyLevel::PreDeclaration);
            }
            const std::wstring property_class_name = get_native_class_name(property_class, false);

            return fmt::format(STR("TSoftObjectPtr<{}>"), property_class_name);
        }

        //Class Properties
        if (field_class_name == STR("ClassProperty") || field_class_name == STR("AssetClassProperty"))
        {
            FClassProperty* class_property = static_cast<FClassProperty*>(property);
            UClass* meta_class = class_property->GetMetaClass();

            if (meta_class == NULL || meta_class == UObject::StaticClass())
            {
                return STR("UClass*");
            }

            if (context.source_file != NULL)
            {
                context.source_file->add_dependency_object(meta_class, DependencyLevel::PreDeclaration);
                context.source_file->add_extra_include(STR("Templates/SubclassOf.h"));
            }
            const std::wstring meta_class_name = get_native_class_name(meta_class, false);

            return fmt::format(STR("TSubclassOf<{}>"), meta_class_name);
        }

        if (auto* class_property = CastField<FClassPtrProperty>(property); class_property)
        {
            // TODO: Confirm that this is accurate
            return STR("TObjectPtr<UClass>");
        }

        if (field_class_name == STR("SoftClassProperty"))
        {
            FSoftClassProperty* soft_class_property = static_cast<FSoftClassProperty*>(property);
            UClass* meta_class = soft_class_property->GetMetaClass();

            if (meta_class == NULL || meta_class == UObject::StaticClass())
            {
                return STR("TSoftClassPtr<UObject>");
            }

            if (context.source_file != NULL)
            {
                context.source_file->add_dependency_object(meta_class, DependencyLevel::PreDeclaration);
            }
            const std::wstring meta_class_name = get_native_class_name(meta_class, false);

            return fmt::format(STR("TSoftClassPtr<{}>"), meta_class_name);
        }

        //Interface Property
        if (field_class_name == STR("InterfaceProperty"))
        {
            FInterfaceProperty* interface_property = static_cast<FInterfaceProperty*>(property);
            UClass* interface_class = interface_property->GetInterfaceClass();

            if (interface_class == NULL || interface_class == UInterface::StaticClass())
            {
                return STR("FScriptInterface");
            }

            if (context.source_file != NULL)
            {
                context.source_file->add_dependency_object(interface_class, DependencyLevel::PreDeclaration);
            }
            const std::wstring interface_class_name = get_native_class_name(interface_class, true);

            return fmt::format(STR("TScriptInterface<{}>"), interface_class_name);
        }

        //Struct Property
        if (field_class_name == STR("StructProperty"))
        {
            FStructProperty* struct_property = static_cast<FStructProperty*>(property);
            UScriptStruct* script_struct = struct_property->GetStruct();

            if (script_struct == NULL)
            {
                throw std::runtime_error(RC::fmt("Struct is NULL for StructProperty %S", property->GetName().c_str()));
            }
            const std::wstring native_struct_name = get_native_struct_name(script_struct);

            if (context.source_file != NULL)
            {
                context.source_file->add_dependency_object(script_struct, DependencyLevel::Include);
            }
            this->m_blueprint_visible_structs.insert(native_struct_name);

            return native_struct_name;
        }

        //Delegate Properties
        if (field_class_name == STR("DelegateProperty"))
        {
            FDelegateProperty* delegate_property = static_cast<FDelegateProperty*>(property);
            UFunction* delegate_signature_function = delegate_property->GetFunctionSignature();

            if (context.source_file != NULL)
            {
                context.source_file->add_dependency_object(delegate_signature_function, DependencyLevel::Include);
            }
            return get_native_delegate_type_name(delegate_signature_function, current_class);
        }

        // In 4.23, they replaced 'MulticastDelegateProperty' with 'Inline' & 'Sparse' variants
        // It looks like the delegate macro might be the same as the 'Inline' variant in later versions, so we'll use the same branch here
        if (field_class_name == STR("MulticastInlineDelegateProperty") || field_class_name == STR("MulticastDelegateProperty"))
        {
            FMulticastInlineDelegateProperty* delegate_property = static_cast<FMulticastInlineDelegateProperty*>(property);
            UFunction* delegate_signature_function = delegate_property->GetFunctionSignature();

            if (context.source_file != NULL)
            {
                context.source_file->add_dependency_object(delegate_signature_function, DependencyLevel::Include);
            }
            return get_native_delegate_type_name(delegate_signature_function, current_class);
        }

        if (field_class_name == STR("MulticastSparseDelegateProperty"))
        {
            FMulticastSparseDelegateProperty* delegate_property = static_cast<FMulticastSparseDelegateProperty*>(property);
            UFunction* delegate_signature_function = delegate_property->GetFunctionSignature();

            if (context.source_file != NULL)
            {
                context.source_file->add_dependency_object(delegate_signature_function, DependencyLevel::Include);
            }
            return get_native_delegate_type_name(delegate_signature_function, current_class);
        }

        //Field path property
        if (field_class_name == STR("FieldPathProperty"))
        {
            FFieldPathProperty* field_path_property = static_cast<FFieldPathProperty*>(property);
            const std::wstring property_class_name = field_path_property->GetPropertyClass()->GetName();
            return fmt::format(STR("TFieldPath<F{}>"), property_class_name);
        }

        //Collection and Map Properties
        // TODO: This is missing support for freeze image array properties because XArrayProperty is incomplete. (low priority)
        if (field_class_name == STR("ArrayProperty"))
        {
            FArrayProperty* array_property = static_cast<FArrayProperty*>(property);
            FProperty* inner_property = array_property->GetInner();

            const std::wstring inner_property_type = generate_property_type_declaration(inner_property, context.inner_context());
            return fmt::format(STR("TArray<{}>"), inner_property_type);
        }

        if (field_class_name == STR("SetProperty"))
        {
            FSetProperty* set_property = static_cast<FSetProperty*>(property);
            FProperty* element_prop = set_property->GetElementProp();

            const std::wstring element_property_type = generate_property_type_declaration(element_prop, context.inner_context());
            return fmt::format(STR("TSet<{}>"), element_property_type);
        }

        // TODO: This is missing support for freeze image map properties because XMapProperty is incomplete. (low priority)
        if (field_class_name == STR("MapProperty"))
        {
            FMapProperty* map_property = static_cast<FMapProperty*>(property);
            FProperty* key_property = map_property->GetKeyProp();
            FProperty* value_property = map_property->GetValueProp();

            const std::wstring key_type = generate_property_type_declaration(key_property, context.inner_context());
            const std::wstring value_type = generate_property_type_declaration(value_property, context.inner_context());

            return fmt::format(STR("TMap<{}, {}>"), key_type, value_type);
        }

        //Standard properties that do not have any special attributes
        if (field_class_name == STR("NameProperty"))
        {
            return STR("FName");
        }
        else if (field_class_name == STR("StrProperty"))
        {
            return STR("FString");
        }
        else if (field_class_name == STR("TextProperty"))
        {
            return STR("FText");
        }
        throw std::runtime_error(RC::fmt("[generate_property_type_declaration] Unsupported property class '%S', full name: '%S'", field_class_name.c_str(), property->GetFullName().c_str()));
    }
    //*/

    auto UEHeaderGenerator::generate_function_argument_flags(FProperty* property) const -> std::wstring
    {
        FlagFormatHelper flag_format_helper{};
        auto property_flags = property->GetPropertyFlags();

        //CPF_ConstParm is handled explicitly in the parameter list generator, it will generate const before parameter
        //if ((PropertyFlags & CPF_ConstParm) != 0) {
        //    FlagFormatHelper.AddSwitch(STR("Const"));
        //}

        //We only want to add UPARAM(Ref) when parameter is marked as reference AND output,
        //while not being marked as constant, because if it's marked as constant, it might be a parameter passed by const reference
        if ((property_flags & CPF_ReferenceParm) != 0 && (property_flags & CPF_OutParm) != 0 && (property_flags & CPF_ConstParm) == 0)
        {
            flag_format_helper.add_switch(STR("Ref"));
        }

        if ((property_flags & CPF_RepSkip) != 0)
        {
            flag_format_helper.add_switch(STR("NotReplicated"));
        }
        return flag_format_helper.build_flag_string();
    }

    auto UEHeaderGenerator::generate_property_flags(FProperty* property) const -> std::wstring
    {
        FlagFormatHelper flag_format_helper{};
        auto property_flags = property->GetPropertyFlags();

        if (UE4SSProgram::settings_manager.UHTHeaderGenerator.MakeAllPropertyBlueprintsReadWrite)
        {
            flag_format_helper.add_switch(STR("EditAnywhere"));
        }
        else if ((property_flags & CPF_Edit) != 0)
        {
            if ((property_flags & CPF_EditConst) != 0)
            {
                if ((property_flags & CPF_DisableEditOnTemplate) != 0)
                {
                    flag_format_helper.add_switch(STR("VisibleInstanceOnly"));
                }
                else if ((property_flags & CPF_DisableEditOnInstance) != 0)
                {
                    flag_format_helper.add_switch(STR("VisibleDefaultsOnly"));
                }
                else
                {
                    flag_format_helper.add_switch(STR("VisibleAnywhere"));
                }
            }
            else
            {
                if ((property_flags & CPF_DisableEditOnTemplate) != 0)
                {
                    flag_format_helper.add_switch(STR("EditInstanceOnly"));
                }
                else if ((property_flags & CPF_DisableEditOnInstance) != 0)
                {
                    flag_format_helper.add_switch(STR("EditDefaultsOnly"));
                }
                else
                {
                    flag_format_helper.add_switch(STR("EditAnywhere"));
                }
            }
        }

        if ((property_flags & CPF_NoClear) != 0)
        {
            flag_format_helper.add_switch(STR("NoClear"));
        }
        if ((property_flags & CPF_EditFixedSize) != 0)
        {
            flag_format_helper.add_switch(STR("EditFixedSize"));
        }
        if ((property_flags & CPF_SimpleDisplay) != 0)
        {
            flag_format_helper.add_switch(STR("SimpleDisplay"));
        }
        if ((property_flags & CPF_AdvancedDisplay) != 0)
        {
            flag_format_helper.add_switch(STR("AdvancedDisplay"));
        }

        if (UE4SSProgram::settings_manager.UHTHeaderGenerator.MakeAllPropertyBlueprintsReadWrite)
        {
            if (property->GetArrayDim() == 1 && is_subtype_valid(property))
            {
                flag_format_helper.add_switch(STR("BlueprintReadWrite"));
            }
            flag_format_helper.get_meta()->add_parameter(STR("AllowPrivateAccess"), STR("true"));
        }
        else if ((property_flags & CPF_BlueprintVisible) != 0)
        {
            if ((property_flags & CPF_BlueprintReadOnly) != 0)
            {
                flag_format_helper.add_switch(STR("BlueprintReadOnly"));
            }
            else
            {
                flag_format_helper.add_switch(STR("BlueprintReadWrite"));
            }
            if ((property_flags & CPF_NativeAccessSpecifierPrivate) != 0)
            {
                flag_format_helper.get_meta()->add_parameter(STR("AllowPrivateAccess"), STR("true"));
            }
        }

        if ((property_flags & CPF_BlueprintAssignable) != 0)
        {
            flag_format_helper.add_switch(STR("BlueprintAssignable"));
        }
        if ((property_flags & CPF_BlueprintCallable) != 0)
        {
            flag_format_helper.add_switch(STR("BlueprintCallable"));
        }
        if ((property_flags & CPF_BlueprintAuthorityOnly) != 0)
        {
            flag_format_helper.add_switch(STR("BlueprintAuthorityOnly"));
        }

        if ((property_flags & CPF_Config) != 0)
        {
            if ((property_flags & CPF_GlobalConfig) != 0)
            {
                flag_format_helper.add_switch(STR("GlobalConfig"));
            }
            else
            {
                flag_format_helper.add_switch(STR("Config"));
            }
        }

        if ((property_flags & CPF_Net) != 0)
        {
            if ((property_flags & CPF_RepNotify) != 0)
            {
                const std::wstring rep_notify_func_name = property->GetRepNotifyFunc().ToString();
                flag_format_helper.add_parameter(STR("ReplicatedUsing"), rep_notify_func_name);
            }
            else
            {
                flag_format_helper.add_switch(STR("Replicated"));
            }
        }
        if ((property_flags & CPF_RepSkip) != 0)
        {
            flag_format_helper.add_switch(STR("NotReplicated"));
        }

        if ((property_flags & CPF_AssetRegistrySearchable) != 0)
        {
            flag_format_helper.add_switch(STR("AssetRegistrySearchable"));
        }
        if ((property_flags & CPF_Interp) != 0)
        {
            flag_format_helper.add_switch(STR("Interp"));
        }
        if ((property_flags & CPF_SaveGame) != 0)
        {
            flag_format_helper.add_switch(STR("SaveGame"));
        }
        if ((property_flags & CPF_NonTransactional) != 0)
        {
            flag_format_helper.add_switch(STR("NonTransactional"));
        }

        if ((property_flags & CPF_Transient) != 0)
        {
            flag_format_helper.add_switch(STR("Transient"));
        }
        if ((property_flags & CPF_DuplicateTransient) != 0)
        {
            flag_format_helper.add_switch(STR("DuplicateTransient"));
        }
        if ((property_flags & CPF_TextExportTransient) != 0)
        {
            flag_format_helper.add_switch(STR("TextExportTransient"));
        }
        if ((property_flags & CPF_NonPIEDuplicateTransient) != 0)
        {
            flag_format_helper.add_switch(STR("NonPIEDuplicateTransient"));
        }
        if ((property_flags & CPF_SkipSerialization) != 0)
        {
            flag_format_helper.add_switch(STR("SkipSerialization"));
        }

        //Need to have all of these flags, otherwise we might accidentally get Instanced of delegate properties; CPF_ExportObject is not set for delegate properties
        uint64_t instanced_flags = CPF_ExportObject | CPF_InstancedReference;
        
        // Instanced Arrays use CPF_ContainsInstancedReference instead of CPF_InstancedReference
        uint64_t instanced_array_flags = CPF_ExportObject | CPF_ContainsInstancedReference;

        if (((property_flags & instanced_flags) == instanced_flags || (property_flags & instanced_array_flags) == instanced_array_flags) &&
            (property->IsA<FObjectProperty>() || (property->IsA<FArrayProperty>() && static_cast<FArrayProperty*>(property)->GetInner()->IsA<FObjectProperty>()) || (property->IsA<FMapProperty>() && static_cast<FMapProperty*>(property)->GetValueProp()->IsA<FObjectProperty>())))
        {
            flag_format_helper.add_switch(STR("Instanced"));
        }
        else if ((property_flags & CPF_ExportObject) != 0)
        {
            flag_format_helper.add_switch(STR("Export"));
        }
        return flag_format_helper.build_flag_string();
    }

    auto UEHeaderGenerator::generate_struct_flags(UScriptStruct* script_struct) const -> std::wstring
    {
        FlagFormatHelper flag_format_helper{};

        UScriptStruct* super_struct = script_struct->GetSuperScriptStruct();
        EStructFlags parent_struct_flags = (EStructFlags)(super_struct ? (super_struct->GetStructFlags() & (~STRUCT_ComputedFlags)) : STRUCT_NoFlags);
        EStructFlags struct_flags = (EStructFlags)(script_struct->GetStructFlags() & (~STRUCT_ComputedFlags));

        EStructFlags struct_own_flags = (EStructFlags)(struct_flags & (~(parent_struct_flags & STRUCT_Inherit)));

        const std::wstring native_struct_name = get_native_struct_name(script_struct);
        if (is_struct_blueprint_type(script_struct) || m_blueprint_visible_structs.contains(native_struct_name) || UE4SSProgram::settings_manager.UHTHeaderGenerator.MakeAllPropertyBlueprintsReadWrite)
        {
            flag_format_helper.add_switch(STR("BlueprintType"));
        }

        if ((struct_own_flags & STRUCT_NoExport) != 0)
        {
            flag_format_helper.add_switch(STR("NoExport"));
        }
        if ((struct_own_flags & STRUCT_Atomic) != 0)
        {
            flag_format_helper.add_switch(STR("Atomic"));
        }
        if ((struct_own_flags & STRUCT_Immutable) != 0)
        {
            flag_format_helper.add_switch(STR("Immutable"));
        }
        return flag_format_helper.build_flag_string();
    }

    auto UEHeaderGenerator::generate_enum_flags(UEnum* uenum) const -> std::wstring
    {
        FlagFormatHelper flag_format_helper{};

        auto enum_flags = uenum->GetEnumFlags();

        if ((((int32_t)enum_flags) & ((int32_t)EEnumFlags::Flags)) != 0)
        {
            flag_format_helper.add_switch(STR("Flags"));
        }

        const std::wstring enum_native_name = get_native_enum_name(uenum);
        if (m_blueprint_visible_enums.contains(enum_native_name))
        {
            flag_format_helper.add_switch(STR("BlueprintType"));
        }
        else if (UE4SSProgram::settings_manager.UHTHeaderGenerator.MakeEnumClassesBlueprintType)
        {
            auto cpp_form = uenum->GetCppForm();
            if (cpp_form == UEnum::ECppForm::EnumClass)
            {
                const auto underlying_type = m_underlying_enum_types.find(enum_native_name);
                if (underlying_type == m_underlying_enum_types.end() || underlying_type->second == STR("uint8"))
                {
                    // Underlying type is implicit or explicitly uint8.
                    flag_format_helper.add_switch(STR("BlueprintType"));
                }
            }
            else
            {
                flag_format_helper.add_switch(STR("BlueprintType"));
            }
        }
        return flag_format_helper.build_flag_string();
    }
        
    auto UEHeaderGenerator::generate_function_flags(UFunction* function, bool is_function_pure_virtual) const -> std::wstring
    {
        FlagFormatHelper flag_format_helper{};

        auto function_flags = function->GetFunctionFlags();

        UObject* outer_object = function->GetOuterPrivate();
        bool is_interface_function = false;

        if (outer_object->GetClassPrivate()->IsChildOf(UClass::StaticClass()))
        {
            UClass* outer_class = (UClass*)outer_object;
            is_interface_function = (outer_class->GetClassFlags() & CLASS_Interface) != 0;
        }

        bool blueprint_callable_added = false;
        if ((function_flags & FUNC_BlueprintCallable) != 0)
        {
            //Interface functions cannot be BlueprintPure
            if ((function_flags & FUNC_BlueprintPure) != 0 && !is_interface_function)
            {
                flag_format_helper.add_switch(STR("BlueprintPure"));
            }
            else
            {
                //If function is marked as FUNC_Const but not as BlueprintPure,
                //it has been explicitly marked as blueprint impure, and we need to preserve this behavior
                if ((function_flags & FUNC_Const) != 0 && !is_interface_function)
                {
                    flag_format_helper.add_parameter(STR("BlueprintPure"), STR("false"));
                }
                flag_format_helper.add_switch(STR("BlueprintCallable"));
                blueprint_callable_added = true;
            }
        }
        if (!blueprint_callable_added && UE4SSProgram::settings_manager.UHTHeaderGenerator.MakeAllFunctionsBlueprintCallable && !is_function_pure_virtual)
        {
            bool has_invalid_param{};
            function->ForEachProperty([&](FProperty* param) {
                if (!is_subtype_valid(param))
                {
                    has_invalid_param = true;
                    return LoopAction::Break;
                }
                else
                {
                    return LoopAction::Continue;
                }
            });

            if (!has_invalid_param)
            {
                flag_format_helper.add_switch(STR("BlueprintCallable"));
            }
        }

        if ((function_flags & FUNC_BlueprintEvent) != 0)
        {
            if ((function_flags & FUNC_Native) != 0)
            {
                flag_format_helper.add_switch(STR("BlueprintNativeEvent"));
            }
            else
            {
                flag_format_helper.add_switch(STR("BlueprintImplementableEvent"));
            }
        }

        if ((function_flags & FUNC_Net) != 0)
        {
            if ((function_flags & FUNC_NetServer) != 0)
            {
                flag_format_helper.add_switch(STR("Server"));
            }
            else if ((function_flags & FUNC_NetClient) != 0)
            {
                flag_format_helper.add_switch(STR("Client"));
            }
            else if ((function_flags & FUNC_NetMulticast) != 0)
            {
                flag_format_helper.add_switch(STR("NetMulticast"));
            }
            else if ((function_flags & FUNC_NetRequest) != 0)
            {
                flag_format_helper.add_switch(STR("ServiceRequest"));
            }
            else if ((function_flags & FUNC_NetResponse) != 0)
            {
                flag_format_helper.add_switch(STR("ServiceResponse"));
            }

            if ((function_flags & FUNC_NetReliable) != 0)
            {
                flag_format_helper.add_switch(STR("Reliable"));
            }
            else
            {
                flag_format_helper.add_switch(STR("Unreliable"));
            }
            if ((function_flags & FUNC_NetValidate) != 0)
            {
                flag_format_helper.add_switch(STR("WithValidation"));
            }
        }

        if ((function_flags & FUNC_Exec) != 0)
        {
            flag_format_helper.add_switch(STR("Exec"));
        }
        if ((function_flags & FUNC_BlueprintAuthorityOnly) != 0)
        {
            flag_format_helper.add_switch(STR("BlueprintAuthorityOnly"));
        }
        if ((function_flags & FUNC_BlueprintCosmetic) != 0)
        {
            flag_format_helper.add_switch(STR("BlueprintCosmetic"));
        }
        
        static auto latent_action_info = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/Engine.LatentActionInfo"));
        bool bWCFound = false;
        bool bLAFound = false;
        function->ForEachProperty([&](FProperty* param) {
            auto param_name = param->GetName();
            auto param_uc_name = string_to_uppercase(param_name);
            if (param_uc_name.find(STR("WORLDCONTEXT")) != param_uc_name.npos)
            {
                flag_format_helper.get_meta()->add_parameter(STR("WorldContext"), std::format(STR("\"{}\""), param_name));
                bWCFound = true;
            }
            if (auto as_struct_property = CastField<FStructProperty>(param); as_struct_property)
            {
                // We now know this is a StructProperty.
                if (as_struct_property->GetStruct()->IsChildOf(latent_action_info))
                {
                  flag_format_helper.get_meta()->add_parameter(STR("LatentInfo"), std::format(STR("\"{}\""), param_name));
                  flag_format_helper.get_meta()->add_switch(STR("Latent"));
                  bLAFound = true;
                }
            }
            if (bWCFound && bLAFound)
            {
                return LoopAction::Break;
            }
            return LoopAction::Continue;
        });
        
        return flag_format_helper.build_flag_string();
    }

    auto UEHeaderGenerator::generate_function_parameter_list(UClass* uclass, UFunction* function, GeneratedSourceFile& header_data, bool generate_comma_before_name, const std::wstring& context_name, const CaseInsensitiveSet& blacklisted_property_names, int32_t* out_num_params) -> std::wstring
    {
        auto function_flags = function->GetFunctionFlags();
        std::wstring function_arguments_string;

        function->ForEachProperty([&](FProperty* property) {
            auto property_flags = property->GetPropertyFlags();
            if ((property_flags & CPF_Parm) != 0 && (property_flags & CPF_ReturnParm) == 0)
            {
                std::wstring param_declaration;

                //We only generate UPARAM declarations if we are not generating the implementation file
                if (!header_data.is_implementation_file())
                {
                    const std::wstring parameter_flags_string = generate_function_argument_flags(property);
                    if (!parameter_flags_string.empty())
                    {
                        param_declaration.append(STR("UPARAM("));
                        param_declaration.append(parameter_flags_string);
                        param_declaration.append(STR(") "));
                    }
                }

                //Force const reference when we're dealing with strings, and they are not passed by reference
                //UHT for whatever reason completely strips const and reference flags from string properties, but happily generates them in boilerplate code
                const bool should_force_const_ref =
                        ((property_flags & (CPF_ReferenceParm | CPF_OutParm)) == 0) &&
                        (property->GetClass().GetName() == STR("StrProperty"));

                //Append const keyword to the parameter declaration if it is marked as const parameter
                if ((property_flags & CPF_ConstParm) != 0 || should_force_const_ref)
                {
                    param_declaration.append(STR("const "));
                }

                PropertyTypeDeclarationContext context(context_name, &header_data);
                param_declaration.append(generate_property_type_declaration(property, context));

                //Reference will be appended if the parameter has a CPF_ReferenceParm flag set,
                //which would also be always set for output parameters
                if ((property_flags & (CPF_ReferenceParm | CPF_OutParm)) != 0 || should_force_const_ref)
                {
                    param_declaration.append(STR("&"));
                }

                if (generate_comma_before_name)
                {
                    param_declaration.append(STR(","));
                }
                param_declaration.append(STR(" "));

                std::wstring property_name = property->GetName();

                //If property name is blacklisted, capitalize first letter and prepend New
                if ((uclass && is_function_parameter_shadowing(uclass, property)) || blacklisted_property_names.contains(property_name))
                {
                    property_name[0] = towupper(property_name[0]);
                    property_name.insert(0, STR("New"));
                }
                param_declaration.append(property_name);

                function_arguments_string.append(param_declaration);
                function_arguments_string.append(STR(", "));
                if (out_num_params) { (*out_num_params)++; }
            }
            return RC::LoopAction::Continue;
        });

        //remove trailing comma and space from the arguments string
        if (!function_arguments_string.empty())
        {
            function_arguments_string.erase(function_arguments_string.size() - 2);
        }
        return function_arguments_string;
    }

    auto UEHeaderGenerator::generate_default_property_value(FProperty* property, GeneratedSourceFile& header_data, const std::wstring& ContextName) -> std::wstring
    {
        const std::wstring field_class_name = property->GetClass().GetName();
        PropertyTypeDeclarationContext context(ContextName, &header_data);

        //Byte Property
        if (field_class_name == STR("ByteProperty"))
        {
            FByteProperty* byte_property = static_cast<FByteProperty*>(property);
            UEnum* Enum = byte_property->GetEnum();

            if (Enum != NULL)
            {
                const int64_t first_enum_constant_value = Enum->GetEnumNameByIndex(0).Value;
                return generate_enum_value(Enum, first_enum_constant_value);
            }
            return STR("0");
        }

        //Enum Property
        if (field_class_name == STR("EnumProperty"))
        {
            FEnumProperty* enum_property = static_cast<FEnumProperty*>(property);
            FNumericProperty* underlying_type = enum_property->GetUnderlyingProperty();
            UEnum* uenum = enum_property->GetEnum();

            if (uenum == NULL)
            {
                throw std::runtime_error(RC::fmt("EnumProperty %S does not have a valid Enum value", property->GetName().c_str()));
            }
            const int64_t first_enum_constant_value = uenum->GetEnumNameByIndex(0).Value;
            return generate_enum_value(uenum, first_enum_constant_value);
        }

        //Bool Property
        if (field_class_name == STR("BoolProperty"))
        {
            return STR("false");
        }
        //Standard Numeric Properties
        if (field_class_name == STR("Int8Property") ||
            field_class_name == STR("Int16Property") ||
            field_class_name == STR("IntProperty") ||
            field_class_name == STR("Int64Property") ||
            field_class_name == STR("UInt16Property") ||
            field_class_name == STR("UInt32Property") ||
            field_class_name == STR("UInt64Property"))
        {
            return STR("0");
        }
        if (field_class_name == STR("FloatProperty"))
        {
            return STR("0.0f");
        }
        if (field_class_name == STR("DoubleProperty"))
        {
            return STR("0.0");
        }

        //Object Properties
        if (field_class_name == STR("ObjectProperty") ||
            field_class_name == STR("WeakObjectProperty") ||
            field_class_name == STR("LazyObjectProperty") ||
            field_class_name == STR("SoftObjectProperty") ||
            field_class_name == STR("AssetObjectProperty") ||
            property->IsA<FObjectPtrProperty>())
        {
            return STR("NULL");
        }

        //Class Properties
        if (field_class_name == STR("ClassProperty") ||
            field_class_name == STR("SoftClassProperty") ||
            field_class_name == STR("InterfaceProperty") ||
            field_class_name == STR("AssetClassProperty") ||
            property->IsA<FClassPtrProperty>())
        {
            return STR("NULL");
        }

        //Struct Property
        if (field_class_name == STR("StructProperty"))
        {
            FStructProperty* struct_property = static_cast<FStructProperty*>(property);
            UScriptStruct* script_struct = struct_property->GetStruct();

            if (script_struct == NULL)
            {
                throw std::runtime_error(RC::fmt("Struct is NULL for StructProperty %S", property->GetName().c_str()));
            }
            const std::wstring native_struct_name = get_native_struct_name(script_struct);
            return fmt::format(STR("{}{{}}"), native_struct_name);
        }

        //Delegate Properties
        if (field_class_name == STR("DelegateProperty") ||
            field_class_name == STR("MulticastInlineDelegateProperty") ||
            field_class_name == STR("MulticastSparseDelegateProperty"))
        {
            const std::wstring delegate_type_name = generate_delegate_name(property, context.context_name);
            return fmt::format(STR("{}()"), delegate_type_name);
        }

        //Field path property
        if (field_class_name == STR("FieldPathProperty"))
        {
            return STR("FFieldPath()");
        }

        //Collection and Map Properties
        if (field_class_name == STR("ArrayProperty"))
        {
            FArrayProperty* array_property = static_cast<FArrayProperty*>(property);
            FProperty* inner_property = array_property->GetInner();

            const std::wstring inner_property_type = generate_property_type_declaration(inner_property, context);
            return fmt::format(STR("TArray<{}>()"), inner_property_type);
        }

        if (field_class_name == STR("SetProperty"))
        {
            FSetProperty* set_property = static_cast<FSetProperty*>(property);
            FProperty* element_prop = set_property->GetElementProp();

            const std::wstring element_property_type = generate_property_type_declaration(element_prop, context);
            return fmt::format(STR("TSet<{}>()"), element_property_type);
        }

        if (field_class_name == STR("MapProperty"))
        {
            FMapProperty* map_property = static_cast<FMapProperty*>(property);
            FProperty* key_property = map_property->GetKeyProp();
            FProperty* value_property = map_property->GetValueProp();

            const std::wstring key_type = generate_property_type_declaration(key_property, context);
            const std::wstring value_type = generate_property_type_declaration(value_property, context);

            return fmt::format(STR("TMap<{}, {}>()"), key_type, value_type);
        }

        //Various string, name and text properties
        if (field_class_name == STR("NameProperty"))
        {
            return STR("NAME_None");
        }
        if (field_class_name == STR("StrProperty"))
        {
            return STR("TEXT(\"\")");
        }
        if (field_class_name == STR("TextProperty"))
        {
            return STR("FText::GetEmpty()");
        }
        throw std::runtime_error(RC::fmt("[generate_default_property_value] Unsupported property class '%S', full name: '%S'", field_class_name.c_str(), property->GetFullName().c_str()));
    }

    auto UEHeaderGenerator::get_class_blueprint_info(UClass* uclass) -> ClassBlueprintInfo
    {
        //These 3 classes are "Intrinsically" blueprintable - they are blueprintable and BlueprintType themselves,
        //but this modifier is not passed through the class hierarchy like normally done
        //So to force generation of correct Blueprintable/BlueprintType statements, we report them as non-blueprint-types
        if (uclass == UObject::StaticClass() || uclass == UActorComponent::StaticClass() || uclass == USceneComponent::StaticClass())
        {
            return ClassBlueprintInfo();
        }

        ClassBlueprintInfo blueprint_info{};
        UClass* super_class = uclass->GetSuperClass();
        if (super_class != NULL)
        {
            blueprint_info = get_class_blueprint_info(super_class);
        }

        uclass->ForEachProperty([&](FProperty* property) {
            auto property_flags = property->GetPropertyFlags();

            if ((property_flags & CPF_BlueprintVisible) != 0)
            {
                blueprint_info.is_blueprint_type = true;
                return RC::LoopAction::Break;
            }
            return RC::LoopAction::Continue;
        });

        uclass->ForEachFunction([&](UFunction* function) {
            auto function_flags = function->GetFunctionFlags();

            if ((function_flags & FUNC_BlueprintEvent) != 0)
            {
                blueprint_info.is_blueprintable = true;
                blueprint_info.is_blueprint_type = true;
                return RC::LoopAction::Break;
            }
            if ((function_flags & FUNC_BlueprintCallable) != 0)
            {
                blueprint_info.is_blueprint_type = true;
            }
            return RC::LoopAction::Continue;
        });
        return blueprint_info;
    }

    auto UEHeaderGenerator::is_struct_blueprint_type(UScriptStruct* script_struct) -> bool
    {
        UScriptStruct* super_struct = script_struct->GetSuperScriptStruct();
        if (super_struct != NULL)
        {
            bool is_super_struct_blueprint_type = is_struct_blueprint_type(super_struct);
            if (is_super_struct_blueprint_type)
            {
                return true;
            }
        }
        bool is_blueprint_type = false;

        script_struct->ForEachProperty([&](FProperty* property) {
            auto property_flags = property->GetPropertyFlags();

            if ((property_flags & CPF_BlueprintVisible) != 0)
            {
                is_blueprint_type = true;
                return RC::LoopAction::Break;
            }
            return RC::LoopAction::Continue;
        });
        return is_blueprint_type;
    }

    auto UEHeaderGenerator::is_function_parameter_shadowing(UClass* uclass, FProperty* function_parameter) -> bool
    {
        bool is_shadowing = false;
        uclass->ForEachPropertyInChain([&](FProperty* property) {
            if (property->GetFName().Equals(function_parameter->GetFName()))
            {
                is_shadowing = true;
                return LoopAction::Break;
            }
            else
            {
                return LoopAction::Continue;
            }
        });

        return is_shadowing;
    }

    auto UEHeaderGenerator::sanitize_enumeration_name(const std::wstring& enumeration_name) -> std::wstring
    {
        std::wstring result_enum_name = enumeration_name;

        //Remove enumeration name from the string
        size_t enum_name_string_split = enumeration_name.find(STR("::"));
        if (enum_name_string_split != std::wstring::npos)
        {
            result_enum_name.erase(0, enum_name_string_split + 2);
        }
        return result_enum_name;
    }

    auto UEHeaderGenerator::get_module_name_for_package(UObject* package) -> std::wstring
    {
        if (package->GetOuterPrivate() != NULL)
        {
            throw std::invalid_argument("Encountered a package with an outer object set");
        }
        std::wstring package_name = package->GetName();
        if (!package_name.starts_with(STR("/Script/")))
        {
            return STR("");
        }
        package_name.erase(0, wcslen(STR("/Script/")));
        return package_name;
    }

    UEHeaderGenerator::UEHeaderGenerator(const FFilePath& root_directory)
    {
        this->m_root_directory = root_directory;
        this->m_primary_module_name = determine_primary_game_module_name();

        //Force inclusion of Core and CoreUObject into all the generated module build files
        this->m_forced_module_dependencies.insert(STR("Core"));
        this->m_forced_module_dependencies.insert(STR("CoreUObject"));
        //TODO not optimal, but still needed for the majority of the cases
        this->m_forced_module_dependencies.insert(STR("Engine"));

        //Add few classes that require explicit UObjectInitializer constructor call
        this->m_classes_with_object_initializer.insert(STR("UUserWidget"));
        this->m_classes_with_object_initializer.insert(STR("UMovieSceneTrack"));
        this->m_classes_with_object_initializer.insert(STR("APlayerStart"));
        this->m_classes_with_object_initializer.insert(STR("URichTextBlock"));
        this->m_classes_with_object_initializer.insert(STR("URichTextBlockImageDecorator"));
        this->m_classes_with_object_initializer.insert(STR("URichTextBlockDecorator"));
    }

    auto UEHeaderGenerator::ignore_selected_modules() -> void
    {
        //Never generate CoreUObject, since it contains a lot of intrinsic classes and is generally always the same
        //Also skip engine initially because engine contents should not change much either

        // Skip "Engine" and "CoreUObject" if requested
        if (UE4SSProgram::settings_manager.UHTHeaderGenerator.IgnoreEngineAndCoreUObject ||
            UE4SSProgram::settings_manager.UHTHeaderGenerator.IgnoreAllCoreEngineModules)
        {
            this->m_ignored_module_names.insert(STR("Engine"));
            this->m_ignored_module_names.insert(STR("CoreUObject"));
        }

        // Skip all core engine packages if requested
        if (UE4SSProgram::settings_manager.UHTHeaderGenerator.IgnoreAllCoreEngineModules)
        {
            this->m_ignored_module_names.insert(STR("ActorLayerUtilities"));
            this->m_ignored_module_names.insert(STR("ActorSequence"));
            this->m_ignored_module_names.insert(STR("AIModule"));
            this->m_ignored_module_names.insert(STR("AndroidPermission"));
            this->m_ignored_module_names.insert(STR("AnimationCore"));
            this->m_ignored_module_names.insert(STR("AnimationSharing"));
            this->m_ignored_module_names.insert(STR("AnimGraphRuntime"));
            this->m_ignored_module_names.insert(STR("AppleImageUtils"));
            this->m_ignored_module_names.insert(STR("ArchVisCharacter"));
            this->m_ignored_module_names.insert(STR("AssetRegistry"));
            this->m_ignored_module_names.insert(STR("AssetTags"));
            this->m_ignored_module_names.insert(STR("AudioAnalyzer"));
            this->m_ignored_module_names.insert(STR("AudioCapture"));
            this->m_ignored_module_names.insert(STR("AudioExtensions"));
            this->m_ignored_module_names.insert(STR("AudioMixer"));
            this->m_ignored_module_names.insert(STR("AudioPlatformConfiguration"));
            this->m_ignored_module_names.insert(STR("AudioSynesthesia"));
            this->m_ignored_module_names.insert(STR("AugmentedReality"));
            this->m_ignored_module_names.insert(STR("AutomationUtils"));
            this->m_ignored_module_names.insert(STR("AvfMediaFactory"));
            this->m_ignored_module_names.insert(STR("BuildPatchServices"));
            this->m_ignored_module_names.insert(STR("CableComponent"));
            this->m_ignored_module_names.insert(STR("Chaos"));
            this->m_ignored_module_names.insert(STR("ChaosCloth"));
            this->m_ignored_module_names.insert(STR("ChaosNiagara"));
            this->m_ignored_module_names.insert(STR("ChaosSolvers"));
            this->m_ignored_module_names.insert(STR("ChaosSolverEngine"));
            this->m_ignored_module_names.insert(STR("CinematicCamera"));
            this->m_ignored_module_names.insert(STR("ClothingSystemRuntimeCommon"));
            this->m_ignored_module_names.insert(STR("ClothingSystemRuntimeInterface"));
            this->m_ignored_module_names.insert(STR("ClothingSystemRuntimeNv"));
            this->m_ignored_module_names.insert(STR("CustomMeshComponent"));
            this->m_ignored_module_names.insert(STR("DatasmithContent"));
            this->m_ignored_module_names.insert(STR("DeveloperSettings"));
            this->m_ignored_module_names.insert(STR("EditableMesh"));
            this->m_ignored_module_names.insert(STR("EngineMessages"));
            this->m_ignored_module_names.insert(STR("EngineSettings"));
            this->m_ignored_module_names.insert(STR("EyeTracker"));
            this->m_ignored_module_names.insert(STR("FacialAnimation"));
            this->m_ignored_module_names.insert(STR("FieldSystemCore"));
            this->m_ignored_module_names.insert(STR("FieldSystemEngine"));
            this->m_ignored_module_names.insert(STR("Foliage"));
            this->m_ignored_module_names.insert(STR("GameplayTags"));
            this->m_ignored_module_names.insert(STR("GameplayTasks"));
            this->m_ignored_module_names.insert(STR("GeometryCache"));
            this->m_ignored_module_names.insert(STR("GeometryCacheTracks"));
            this->m_ignored_module_names.insert(STR("GeometryCollectionCore"));
            this->m_ignored_module_names.insert(STR("GeometryCollectionSimulationCore"));
            this->m_ignored_module_names.insert(STR("GeometryCollectionEngine"));
            this->m_ignored_module_names.insert(STR("GeometryCollectionTracks"));
            this->m_ignored_module_names.insert(STR("GooglePAD"));
            this->m_ignored_module_names.insert(STR("HeadMountedDisplay"));
            this->m_ignored_module_names.insert(STR("ImageWrapper"));
            this->m_ignored_module_names.insert(STR("ImageWriteQueue"));
            this->m_ignored_module_names.insert(STR("ImgMedia"));
            this->m_ignored_module_names.insert(STR("ImgMediaFactory"));
            this->m_ignored_module_names.insert(STR("InputCore"));
            this->m_ignored_module_names.insert(STR("InteractiveToolsFramework"));
            this->m_ignored_module_names.insert(STR("JsonUtilities"));
            this->m_ignored_module_names.insert(STR("Landscape"));
            this->m_ignored_module_names.insert(STR("LevelSequence"));
            this->m_ignored_module_names.insert(STR("LightPropagationVolumeRuntime"));
            this->m_ignored_module_names.insert(STR("LiveLinkInterface"));
            this->m_ignored_module_names.insert(STR("LocationServicesBPLibrary"));
            this->m_ignored_module_names.insert(STR("LuminRuntimeSettings"));
            this->m_ignored_module_names.insert(STR("MagicLeap"));
            this->m_ignored_module_names.insert(STR("MagicLeapAR"));
            this->m_ignored_module_names.insert(STR("MagicLeapARPin"));
            this->m_ignored_module_names.insert(STR("MagicLeapAudio"));
            this->m_ignored_module_names.insert(STR("MagicLeapController"));
            this->m_ignored_module_names.insert(STR("MagicLeapEyeTracker"));
            this->m_ignored_module_names.insert(STR("MagicLeapHandMeshing"));
            this->m_ignored_module_names.insert(STR("MagicLeapHandTracking"));
            this->m_ignored_module_names.insert(STR("MagicLeapIdentity"));
            this->m_ignored_module_names.insert(STR("MagicLeapImageTracker"));
            this->m_ignored_module_names.insert(STR("MagicLeapLightEstimation"));
            this->m_ignored_module_names.insert(STR("MagicLeapPlanes"));
            this->m_ignored_module_names.insert(STR("MagicLeapPrivileges"));
            this->m_ignored_module_names.insert(STR("MagicLeapSecureStorage"));
            this->m_ignored_module_names.insert(STR("MagicLeapSharedWorld"));
            this->m_ignored_module_names.insert(STR("MaterialShaderQualitySettings"));
            this->m_ignored_module_names.insert(STR("MediaAssets"));
            this->m_ignored_module_names.insert(STR("MediaCompositing"));
            this->m_ignored_module_names.insert(STR("MediaUtils"));
            this->m_ignored_module_names.insert(STR("MeshDescription"));
            this->m_ignored_module_names.insert(STR("MobilePatchingUtils"));
            this->m_ignored_module_names.insert(STR("MotoSynth"));
            this->m_ignored_module_names.insert(STR("MoviePlayer"));
            this->m_ignored_module_names.insert(STR("MovieScene"));
            this->m_ignored_module_names.insert(STR("MovieSceneCapture"));
            this->m_ignored_module_names.insert(STR("MovieSceneTracks"));
            this->m_ignored_module_names.insert(STR("MRMesh"));
            this->m_ignored_module_names.insert(STR("NavigationSystem"));
            this->m_ignored_module_names.insert(STR("NetCore"));
            this->m_ignored_module_names.insert(STR("Niagara"));
            this->m_ignored_module_names.insert(STR("NiagaraAnimNotifies"));
            this->m_ignored_module_names.insert(STR("NiagaraCore"));
            this->m_ignored_module_names.insert(STR("NiagaraShader"));
            this->m_ignored_module_names.insert(STR("OculusHMD"));
            this->m_ignored_module_names.insert(STR("OculusInput"));
            this->m_ignored_module_names.insert(STR("OculusMR"));
            this->m_ignored_module_names.insert(STR("OnlineSubsystem"));
            this->m_ignored_module_names.insert(STR("OnlineSubsystemUtils"));
            this->m_ignored_module_names.insert(STR("Overlay"));
            this->m_ignored_module_names.insert(STR("PacketHandler"));
            this->m_ignored_module_names.insert(STR("Paper2D"));
            this->m_ignored_module_names.insert(STR("PhysicsCore"));
            this->m_ignored_module_names.insert(STR("PhysXVehicles"));
            this->m_ignored_module_names.insert(STR("ProceduralMeshComponent"));
            this->m_ignored_module_names.insert(STR("PropertyAccess"));
            this->m_ignored_module_names.insert(STR("PropertyPath"));
            this->m_ignored_module_names.insert(STR("Renderer"));
            this->m_ignored_module_names.insert(STR("Serialization"));
            this->m_ignored_module_names.insert(STR("SessionMessages"));
            this->m_ignored_module_names.insert(STR("SignificanceManager"));
            this->m_ignored_module_names.insert(STR("Slate"));
            this->m_ignored_module_names.insert(STR("SlateCore"));
            this->m_ignored_module_names.insert(STR("SoundFields"));
            this->m_ignored_module_names.insert(STR("StaticMeshDescription"));
            this->m_ignored_module_names.insert(STR("SteamVR"));
            this->m_ignored_module_names.insert(STR("SteamVRInputDevice"));
            this->m_ignored_module_names.insert(STR("Synthesis"));
            this->m_ignored_module_names.insert(STR("TcpMessaging"));
            this->m_ignored_module_names.insert(STR("TemplateSequence"));
            this->m_ignored_module_names.insert(STR("TimeManagement"));
            this->m_ignored_module_names.insert(STR("UdpMessaging"));
            this->m_ignored_module_names.insert(STR("UMG"));
            this->m_ignored_module_names.insert(STR("UObjectPlugin"));
            this->m_ignored_module_names.insert(STR("VariantManagerContent"));
            this->m_ignored_module_names.insert(STR("VectorVM"));
            this->m_ignored_module_names.insert(STR("WmfMediaFactory"));
        }
    }

    auto UEHeaderGenerator::dump_native_packages() -> void
    {
        ignore_selected_modules();

        Output::send(STR("Cleaning up previously generated SDK (if one exists)\n"));
        if (std::filesystem::exists(m_root_directory))
        {
            std::filesystem::remove_all(m_root_directory);
        }

        Output::send(STR("Initializing native packages dump\n"));

        std::vector<UClass*> native_classes_to_dump;
        std::vector<UScriptStruct*> native_structs_to_dump;
        std::vector<UEnum*> native_enums_to_dump;
        std::vector<UFunction*> native_delegates_to_dump;

        Output::send(STR("Gathering native objects for dumping\n"));
        UObjectGlobals::ForEachUObject([&](void* raw_object, int32_t chunk_index, int32_t object_index) {
            UObject* typed_object = static_cast<UObject*>(raw_object);

            if (UClass* casted_object = Cast<UClass>(typed_object))
            {
                if ((casted_object->GetClassFlags() & CLASS_Native) != 0)
                {
                    native_classes_to_dump.push_back(casted_object);
                }
            }
            else if (UScriptStruct* casted_struct = Cast<UScriptStruct>(typed_object))
            {
                if ((casted_struct->GetStructFlags() & STRUCT_Native) != 0)
                {
                    native_structs_to_dump.push_back(casted_struct);
                }
            }
            else if (UEnum* uenum = Cast<UEnum>(typed_object))
            {
                if (!typed_object->IsA<UUserDefinedEnum>())
                {
                    native_enums_to_dump.push_back(uenum);
                }
            }
            else if (UFunction* function = Cast<UFunction>(typed_object))
            {
                //We are looking for delegate signature functions located inside the native packages
                //When they are located directly on the top level, they will result in a separate header, otherwise they will
                //be included into their respective outer header
                if (is_delegate_signature_function(function) && function->GetOuterPrivate()->IsA<UPackage>())
                {
                    UPackage* outer_package = Cast<UPackage>(function->GetOuterPrivate());

                    //Make sure the function package actually corresponds to the real native module
                    if (!get_module_name_for_package(outer_package).empty()) {
                        native_delegates_to_dump.push_back(function);
                    }
                }
            }

            return RC::LoopAction::Continue;
        });

        Output::send(STR("Attempting to dump {} native classes\n"), native_classes_to_dump.size());

        for (UFunction* delegate_signature_function : native_delegates_to_dump)
        {
            //Output::send(STR("Dumping native delegate type {}\n"), global_delegate_signature->GetName());
            generate_object_description_file(delegate_signature_function);
        }

        for (UClass* class_to_dump : native_classes_to_dump)
        {
            //Output::send(STR("Dumping native class {}\n"), class_to_dump->GetName());
            generate_object_description_file(class_to_dump);
        }

        Output::send(STR("Attempting to dump {} native structs\n"), native_structs_to_dump.size());

        for (UScriptStruct* struct_to_dump : native_structs_to_dump)
        {
            //Output::send(STR("Dumping native struct {}\n"), struct_to_dump->GetName());
            generate_object_description_file(struct_to_dump);
        }

        Output::send(STR("Attempting to dump {} native enums\n"), native_enums_to_dump.size());

        for (UEnum* enum_to_dump : native_enums_to_dump)
        {
            //Output::send(STR("Dumping native enum {}\n"), enum_to_dump->GetName());
            generate_object_description_file(enum_to_dump);
        }

        Output::send(STR("Writing stub module build files for {} modules\n"), m_module_dependencies.size());
        for (const auto& module_pair : m_module_dependencies)
        {
            generate_module_implementation_file(module_pair.first);
            generate_module_build_file(module_pair.first);
        }

        // Pass #2
        for (auto& header_file : m_header_files)
        {
            auto object = header_file.get_corresponding_object();
            bool is_struct = object->IsA<UStruct>();
            bool is_class = object->IsA<UClass>();
            if ((is_struct || is_class) && m_structs_that_need_get_type_hash.find(std::bit_cast<UStruct*>(object)) != m_structs_that_need_get_type_hash.end())
            {
                File::StringType name{};
                if (is_class)
                {
                    name = get_native_class_name(std::bit_cast<UClass*>(object));
                }
                else if (is_struct)
                {
                    name = get_native_struct_name(std::bit_cast<UScriptStruct*>(object));
                }
                header_file.append_line(std::format(STR("FORCEINLINE uint32 GetTypeHash(const {}) {{ return 0; }}"), name));
            }
            header_file.serialize_file_content_to_disk();
        }

        Output::send(STR("Done!\n"));
    }

    auto UEHeaderGenerator::generate_object_description_file(UObject* object) -> bool
    {
        const std::wstring module_name = get_module_name_for_package(object->GetOutermost());
        const std::wstring file_base_name = get_header_name_for_object(object);

        if (module_name.empty())
        {
            return false;
        }
        if (m_ignored_module_names.contains(module_name))
        {
            return false;
        }

        GeneratedSourceFile& header_file = m_header_files.emplace_back(GeneratedSourceFile::create_source_file(m_root_directory, module_name, file_base_name, false, object));
        GeneratedSourceFile implementation_file = GeneratedSourceFile::create_source_file(m_root_directory, module_name, file_base_name, true, object);
        implementation_file.set_header_file(&header_file);

        if (UClass* uclass = Cast<UClass>(object))
        {
            if (uclass->IsChildOf<UInterface>())
            {
                generate_interface_definition(uclass, header_file);
            }
            else
            {
                generate_object_definition(uclass, header_file);
                generate_object_implementation(uclass, implementation_file);
            }
        }
        else if (UScriptStruct* script_struct = Cast<UScriptStruct>(object))
        {
            generate_struct_definition(script_struct, header_file);
            generate_struct_implementation(script_struct, implementation_file);
        }
        else if (UEnum* uenum = Cast<UEnum>(object))
        {
            generate_enum_definition(uenum, header_file);
        }
        else if (UFunction* function = Cast<UFunction>(object))
        {
            if (!is_delegate_signature_function(function))
            {
                throw std::runtime_error(RC::fmt("Function %S is not a delegate signature function", function->GetName().c_str()));
            }
            if (!function->GetOuterPrivate()->IsA<UPackage>())
            {
                throw std::runtime_error(RC::fmt("Delegate Signature Function %S does not have a UPackage as it's owner", function->GetName().c_str()));
            }
            generate_global_delegate_declaration(function, header_file);
        }
        else
        {
            throw std::runtime_error(RC::fmt("Provided object %S is not of a supported type: %S", object->GetName().c_str(), object->GetClassPrivate()->GetName().c_str()));
        }

        auto iterator = this->m_module_dependencies.find(module_name);
        if (iterator == this->m_module_dependencies.end())
        {
            iterator = this->m_module_dependencies.insert({module_name, std::make_shared<std::set<std::wstring>>()}).first;
        }

        if (!header_file.has_content_to_save())
        {
            return false;
        }
        implementation_file.serialize_file_content_to_disk();

        //Record module names used in the headers
        std::shared_ptr<std::set<std::wstring>> out_dependency_module_names = iterator->second;
        header_file.copy_dependency_module_names(*out_dependency_module_names);
        implementation_file.copy_dependency_module_names(*out_dependency_module_names);

        return true;
    }

    auto UEHeaderGenerator::generate_object_pre_declaration(UObject* object, std::wstring& out_extra_declaration) -> std::wstring
    {
        UClass* object_class = object->GetClassPrivate();

        if (object_class->IsChildOf(UClass::StaticClass()))
        {
            UClass* uclass = static_cast<UClass*>(object);

            if (uclass->IsChildOf(UInterface::StaticClass()))
            {
                out_extra_declaration = fmt::format(STR("class {};"), get_native_class_name(uclass, true));
            }
            return fmt::format(STR("class {};"), get_native_class_name(uclass, false));

        }
        else if (object_class->IsChildOf(UScriptStruct::StaticClass()))
        {
            UScriptStruct* script_struct = static_cast<UScriptStruct*>(object);

            return fmt::format(STR("struct {};"), get_native_struct_name(script_struct));

        }
        else if (object_class->IsChildOf(UEnum::StaticClass()))
        {
            UEnum* uenum = static_cast<UEnum*>(object);

            //TODO do we want them? They're not that easy since we do not know enum types precisely in advance
            throw std::invalid_argument("Enum pre-declarations are not supported");
        }
        else
        {
            throw std::invalid_argument("Provided object is not of a supported type, should be UClass/UScriptStruct/UEnum");
        }
    }

    auto UEHeaderGenerator::get_header_name_for_object(UObject* object, bool get_existing_header) -> std::wstring {
        File::StringType header_name{};
        UObject* final_object{};

        if (object->IsA<UClass>() || object->IsA<UScriptStruct>()) {
            //Class and struct headers follow the relevant object name
            header_name = object->GetName();
            final_object = object;
        } else if (object->IsA<UEnum>()) {
            //Enumeration usually have the E prefix which will be present in the header names
            //We do not strip it because there are some broken headers that do not follow that convention (e.g. funny Wwise)
            header_name = object->GetName();
            final_object = object;
        } else {
            //Delegate signature functions need to have their postfix removed
            UFunction* signature_function = Unreal::Cast<UFunction>(object);
            if (signature_function != nullptr && is_delegate_signature_function(signature_function))
            {

                //If delegate is not declared inside the package, it is declared in the header of it's outer
                if (!signature_function->GetOuterPrivate()->IsA<UPackage>())
                {
                    final_object = signature_function->GetOuterPrivate();
                    header_name = get_header_name_for_object(final_object, get_existing_header);
                }
                else
                {
                    //Otherwise, remove the postfix and use the function name as the header name
                    //Also append the delegate postfix because apparently there can be conflicts
                    std::wstring DelegateName = strip_delegate_signature_postfix(signature_function);
                    DelegateName.append(STR("Delegate"));
                    header_name = DelegateName;
                    final_object = object;
                }
            }
        }

        if (header_name.empty())
        {
            //Unsupported dependency object type
            throw std::runtime_error(RC::fmt("Unsupported dependency object type %S: %S", object->GetClassPrivate()->GetName().c_str(), object->GetName().c_str()));
        }

        if (get_existing_header)
        {
            if (auto it2 = m_dependency_object_to_unique_id.find(final_object); it2 != m_dependency_object_to_unique_id.end())
            {
                header_name.append(std::format(STR("{}"), it2->second));
            }
        }
        else
        {
            if (auto it = m_used_file_names.find(header_name); it != m_used_file_names.end())
            {
                header_name.append(std::format(STR("{}"), ++it->second.usable_id));
                m_dependency_object_to_unique_id.emplace(final_object, it->second.usable_id);
            }
            else
            {
                m_used_file_names.emplace(header_name, UniqueName{header_name});
            }
        }

        return header_name;
    }

    auto UEHeaderGenerator::generate_global_delegate_declaration(UFunction* signature_function, GeneratedSourceFile& header_data) -> void {
        generate_delegate_type_declaration(signature_function, header_data);
    }

    auto UEHeaderGenerator::determine_primary_game_module_name() -> std::wstring {
        HMODULE primary_executable_module = GetModuleHandleW(NULL);
        wchar_t module_name_buffer[1024] {'\0'};
        GetModuleFileNameW(primary_executable_module, module_name_buffer, ARRAYSIZE(module_name_buffer));

        //Retrieve the filename from the full path, strip down the extension
        FFilePath root_executable_path((std::wstring(module_name_buffer)));
        std::wstring filename = root_executable_path.filename().replace_extension().wstring();

        //Remove the shipping file postfix
        std::wstring shipping_postfix = STR("-Win64-Shipping");
        if (filename.ends_with(shipping_postfix)) {
            filename.erase(filename.length() - shipping_postfix.length());
        }
        return filename;
    }

    auto UEHeaderGenerator::generate_cross_module_include(UObject* object, const std::wstring& module_name, const std::wstring& fallback_name) -> std::wstring {
        //Retrieve the most top level object located inside the native package
        UObject* top_level_object = object;

        while (!top_level_object->GetOuterPrivate()->IsA<UPackage>()) {
            top_level_object = top_level_object->GetOuterPrivate();
        }

        const std::wstring object_name = top_level_object->GetName();
        return fmt::format(STR("//CROSS-MODULE INCLUDE V2: -ModuleName={} -ObjectName={} -FallbackName={}\n"), module_name, object_name, fallback_name);
    }

    GeneratedFile::GeneratedFile(const FFilePath& full_file_path)
    {
        this->m_full_file_path = full_file_path;
        this->m_file_base_name = full_file_path.filename().replace_extension().wstring();
        this->m_current_indent_count = 0;
    }

    auto GeneratedFile::append_line(const std::wstring& line) -> void
    {
        for (int32_t i = 0; i < m_current_indent_count; i++)
        {
            m_file_contents_buffer.append(STR("    "));
        }
        m_file_contents_buffer.append(line);
        m_file_contents_buffer.append(STR("\n"));
    }

    auto GeneratedFile::append_line_no_indent(const std::wstring& line) -> void
    {
        m_file_contents_buffer.append(line);
        m_file_contents_buffer.append(STR("\n"));
    }

    auto GeneratedFile::begin_indent_level() -> void
    {
        m_current_indent_count++;
    }

    auto GeneratedFile::end_ident_level() -> void
    {
        m_current_indent_count--;
        if (m_current_indent_count < 0)
        {
            throw std::invalid_argument("Attempt to pop empty ident level stack");
        }
    }

    auto GeneratedFile::serialize_file_content_to_disk() -> bool
    {
        if (!has_content_to_save())
        {
            return false;
        }
        //TODO might be slow, maybe move it out into the header generator?
        std::filesystem::create_directories(this->m_full_file_path.parent_path());

        std::wofstream file_output_stream;
        file_output_stream.open(m_full_file_path);
        if (!file_output_stream.is_open())
        {
            throw std::invalid_argument("Failed to open the header file");
        }
        file_output_stream << generate_file_contents();
        file_output_stream.close();
        return true;
    }

    auto GeneratedFile::generate_file_contents() -> std::wstring
    {
        return m_file_contents_buffer;
    }

    auto GeneratedFile::has_content_to_save() const -> bool
    {
        return !m_file_contents_buffer.empty();
    }

    auto GeneratedSourceFile::create_source_file(const FFilePath& root_dir, const std::wstring& module_name, const std::wstring& base_name, bool is_implementation_file, UObject* object) -> GeneratedSourceFile
    {
        FFilePath full_file_path;
        if (is_implementation_file)
        {
            full_file_path = root_dir / module_name / STR("Private") / (base_name + STR(".cpp"));
        }
        else
        {
            full_file_path = root_dir / module_name / STR("Public") / (base_name + STR(".h"));
        }
        return GeneratedSourceFile(full_file_path, module_name, is_implementation_file, object);
    }

    GeneratedSourceFile::GeneratedSourceFile(const FFilePath& file_path, const std::wstring& file_module_name, bool is_implementation_file, UObject* object) : GeneratedFile(file_path)
    {
        this->m_file_module_name = file_module_name;
        this->m_is_implementation_file = is_implementation_file;
        this->m_object = object;
    }

    auto GeneratedSourceFile::set_header_file(GeneratedSourceFile* header_file) -> void
    {
        this->m_header_file = header_file;
    }

    auto GeneratedSourceFile::add_extra_include(const std::wstring& included_file_name) -> void
    {
        this->m_extra_includes.insert(included_file_name);
    }

    auto GeneratedSourceFile::add_dependency_object(UObject* object, DependencyLevel dependency_level) -> void
    {
        const auto iterator = m_dependencies.find(object);

        //Only add the dependency if we don't have one already or requested level is higher than the current one
        if (iterator == m_dependencies.end() || (int32_t)iterator->second <= (int32_t)dependency_level)
        {
            m_dependencies.insert_or_assign(object, dependency_level);
        }
    }

    auto GeneratedSourceFile::generate_file_contents() -> std::wstring
    {
        std::wstring result_header_contents;
        result_header_contents.append(generate_includes_string());
        result_header_contents.append(STR("\n"));

        std::wstring pre_declarations_string = generate_pre_declarations_string();
        if (!pre_declarations_string.empty())
        {
            result_header_contents.append(pre_declarations_string);
            result_header_contents.append(STR("\n"));
        }

        if (!m_file_contents_buffer.empty())
        {
            result_header_contents.append(m_file_contents_buffer);
            result_header_contents.append(STR("\n"));
        }
        return result_header_contents;
    }

    auto GeneratedSourceFile::generate_includes_string() const -> std::wstring
    {
        std::wstring result_include_string;

        //For the header file, we generate the pragma and minimal core includes
        if (!m_is_implementation_file)
        {
            result_include_string.append(STR("#pragma once\n"));
            result_include_string.append(STR("#include \"CoreMinimal.h\"\n"));
        }
        //For CPP implementation file, we need to generate the header include
        if (m_is_implementation_file)
        {
            if (m_header_file != NULL)
            {
                //Generate it if we have the correct header file set
                result_include_string.append(fmt::format(STR("#include \"{}.h\"\n"), m_header_file->m_file_base_name));
            }
            else
            {
                //Otherwise, we generate a simple minimal core include
                result_include_string.append(STR("#include \"CoreMinimal.h\"\n"));
            }
        }

        //Generate extra includes we might need that do not represent objects
        for (const std::wstring& extra_included_file : m_extra_includes)
        {
            result_include_string.append(fmt::format(STR("#include \"{}\"\n"), extra_included_file));
        }

        //Generate includes for the relevant object files
        for (const auto& dependency_pair : m_dependencies)
        {
            UObject* dependency_object = dependency_pair.first;

            //Only want to generate include dependencies
            if (dependency_pair.second != DependencyLevel::Include)
            {
                continue;
            }

            const std::wstring object_header_name = UEHeaderGenerator::get_header_name_for_object(dependency_object, true);

            //Definitely skip include if object in question is placed into this header
            if (object_header_name == m_file_base_name)
            {
                continue;
            }

            //Skip includes that have already been generated on the header file
            if (m_is_implementation_file && m_header_file != NULL)
            {
                if (m_header_file->has_dependency(dependency_object, DependencyLevel::Include))
                {
                    continue;
                }
            }
            UObject* package = dependency_object->GetOutermost();
            std::wstring native_module_name = UEHeaderGenerator::get_module_name_for_package(package);

            if (!native_module_name.empty())
            {
                //If this package corresponds to the file inside this module, we generate the normal include,
                //since generated headers are always located in the module root and follow one file per object convention
                if (m_file_module_name == native_module_name)
                {
                    result_include_string.append(fmt::format(STR("#include \"{}.h\"\n"), object_header_name));
                }
                else
                {
                    //Otherwise, we generate an include stub which will be handled by the unreal engine commandlet later
                    m_dependency_module_names.insert(native_module_name);
                    result_include_string.append(UEHeaderGenerator::generate_cross_module_include(dependency_object, native_module_name, object_header_name));
                }
            }
        }

        //Last include of the header file should always be a generated one
        if (!m_is_implementation_file)
        {
            result_include_string.append(fmt::format(STR("#include \"{}.generated.h\"\n"), m_file_base_name));
        }
        return result_include_string;
    }

    auto GeneratedSourceFile::generate_pre_declarations_string() const -> std::wstring
    {
        std::wstring result_declarations;

        //Generate pre-declarations for the relevant object files
        for (const auto& dependency_pair : m_dependencies)
        {
            UObject* dependency_object = dependency_pair.first;

            //Only want to generate pre-declarations
            if (dependency_pair.second != DependencyLevel::PreDeclaration)
            {
                continue;
            }

            //We still need to reference the object's owner module
            UObject* package = dependency_object->GetOutermost();
            std::wstring native_module_name = UEHeaderGenerator::get_module_name_for_package(package);

            if (!native_module_name.empty() && m_file_module_name != native_module_name)
            {
                m_dependency_module_names.insert(native_module_name);
            }

            std::wstring extra_declaration_string;
            std::wstring declaration_string = UEHeaderGenerator::generate_object_pre_declaration(dependency_object, extra_declaration_string);

            if (!declaration_string.empty())
            {
                result_declarations.append(declaration_string);
                result_declarations.append(STR("\n"));
            }
            if (!extra_declaration_string.empty())
            {
                result_declarations.append(extra_declaration_string);
                result_declarations.append(STR("\n"));
            }
        }
        return result_declarations;
    }

    auto GeneratedSourceFile::has_content_to_save() const -> bool
    {
        return !m_file_contents_buffer.empty();
    }

    auto GeneratedSourceFile::has_dependency(UObject* object, DependencyLevel dependency_level) -> bool
    {
        const auto iterator = m_dependencies.find(object);
        return iterator != m_dependencies.end() &&
               ((int32_t)iterator->second >= (int32_t)dependency_level);
    }
}
