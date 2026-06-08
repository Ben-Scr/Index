#include "pch.hpp"
#include "Assets/AssetRegistry.hpp"
#include "Components/General/NameComponent.hpp"
#include "Core/Application.hpp"
#include "Editor/ExternalEditor.hpp"
#include "Gui/ImGuiImplWebGPU.hpp"
#include "Gui/ImGuiUtils.hpp"
#include "Inspector/PropertyDescriptor.hpp"
#include "Inspector/PropertyDrawer.hpp"
#include "Inspector/PropertyType.hpp"
#include "Inspector/PropertyValue.hpp"
#include "Inspector/ReferencePicker.hpp"
#include "Project/ProjectManager.hpp"
#include "Scene/ComponentRegistry.hpp"
#include "Scene/Scene.hpp"
#include "Scene/SceneManager.hpp"
#include "Scripting/ScriptComponent.hpp"
#include "Scripting/ScriptComponentInspector.hpp"
#include "Scripting/ScriptDiscovery.hpp"
#include "Scripting/ScriptEngine.hpp"
#include "Scripting/DataAssetManager.hpp"
#include "Scripting/ScriptSystem.hpp"
#include "Serialization/Json.hpp"
#include "Serialization/Path.hpp"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Index {

	namespace {

		// JSON field record produced by C# Index-ScriptCore (one per
		// [ShowInEditor] field). The C++ side parses this into a
		// PropertyDescriptor and drives the unified PropertyDrawer.
		struct EditorFieldRecord {
			std::string Name;
			std::string DisplayName;
			std::string TypeTag;             // "float", "enum", "component:Foo", ...
			std::string ItemTypeTag;         // for TypeTag == "list": the element's wire tag
			std::string ComponentTypeName;   // populated when TypeTag starts with "component:"
			std::string Value;                // current value as a string (PropertyValue::FromString accepts this)
			std::string Tooltip;
			std::string HeaderContent;
			int HeaderSize = 0;
			bool ReadOnly = false;
			bool HasClamp = false;
			float ClampMin = 0.0f;
			float ClampMax = 0.0f;
			bool HasSpace = false;
			float SpaceHeight = 0.0f;
			// EnabledIf metadata from [EnabledIf("Field", expectedValue)].
			// EnabledIfAny == true means "enable when the named field is
			// truthy" (no expected value supplied to the attribute).
			bool HasEnabledIf = false;
			bool EnabledIfAny = false;
			std::string EnabledIfField;
			std::string EnabledIfValue;
			std::shared_ptr<EnumDescriptor> Enum;
		};

		std::string ToLowerCopy(std::string value) {
			std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
				return static_cast<char>(std::tolower(ch));
			});
			return value;
		}

		// Renders the "open script" button for all inspector types; shows the on-disk filename, falls back to fallbackLabel when discovery can't locate the file.
		template <typename Predicate>
		void DrawOpenScriptButton(const std::string& className, Predicate predicate,
			const std::string& fallbackLabel) {
			std::filesystem::path scriptPath;
			std::vector<EditorScriptDiscovery::ScriptEntry> entries;
			EditorScriptDiscovery::CollectProjectScriptEntries(entries);
			for (const auto& entry : entries) {
				if (entry.ClassName == className && predicate(entry)) {
					scriptPath = entry.Path;
					break;
				}
			}
			const std::string buttonText = scriptPath.empty()
				? fallbackLabel
				: scriptPath.filename().string();

			ImGuiUtils::DrawInspectorValue("Script", [&buttonText, &scriptPath](const char* id) {
				ImGui::Button((buttonText + id).c_str(),
					ImVec2(ImGui::GetContentRegionAvail().x, 0.0f));
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Double-click to open script");
					if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !scriptPath.empty()) {
						ExternalEditor::OpenFile(scriptPath.string());
					}
				}
			});
		}

		PropertyType ResolveScriptFieldType(const EditorFieldRecord& rec) {
			if (rec.TypeTag.rfind("component:", 0) == 0) return PropertyType::ComponentRef;
			if (rec.TypeTag.rfind("dataasset:", 0) == 0) return PropertyType::AssetRef;
			if (rec.TypeTag == "entity") return PropertyType::EntityRef;
			return PropertyTypeFromString(rec.TypeTag);
		}

		std::vector<EditorFieldRecord> ParseEditorFields(const char* json) {
			std::vector<EditorFieldRecord> fields;
			if (!json || !*json) return fields;

			Json::Value root;
			std::string parseError;
			if (!Json::TryParse(json, root, &parseError) || !root.IsArray()) {
				IDX_CORE_WARN_TAG("ScriptInspector", "Failed to parse editor field metadata: {}", parseError);
				return fields;
			}

			for (const Json::Value& item : root.GetArray()) {
				if (!item.IsObject()) continue;
				EditorFieldRecord rec;

				if (const Json::Value* v = item.FindMember("name"))           rec.Name = v->AsStringOr();
				if (const Json::Value* v = item.FindMember("displayName"))    rec.DisplayName = v->AsStringOr();
				if (const Json::Value* v = item.FindMember("type"))           rec.TypeTag = v->AsStringOr();
				if (const Json::Value* v = item.FindMember("itemType"))       rec.ItemTypeTag = v->AsStringOr();
				if (const Json::Value* v = item.FindMember("value"))          rec.Value = v->AsStringOr();
				if (const Json::Value* v = item.FindMember("readOnly"))       rec.ReadOnly = v->AsBoolOr(false);
				if (const Json::Value* v = item.FindMember("hasClamp"))       rec.HasClamp = v->AsBoolOr(false);
				if (const Json::Value* v = item.FindMember("clampMin"))       rec.ClampMin = static_cast<float>(v->AsDoubleOr(0.0));
				if (const Json::Value* v = item.FindMember("clampMax"))       rec.ClampMax = static_cast<float>(v->AsDoubleOr(0.0));
				if (const Json::Value* v = item.FindMember("tooltip"))        rec.Tooltip = v->AsStringOr();
				if (const Json::Value* v = item.FindMember("headerContent"))  rec.HeaderContent = v->AsStringOr();
				if (const Json::Value* v = item.FindMember("headerSize"))     rec.HeaderSize = static_cast<int>(v->AsDoubleOr(0.0));
				if (const Json::Value* v = item.FindMember("hasSpace"))       rec.HasSpace = v->AsBoolOr(false);
				if (const Json::Value* v = item.FindMember("spaceHeight"))    rec.SpaceHeight = static_cast<float>(v->AsDoubleOr(0.0));
				if (const Json::Value* v = item.FindMember("hasEnabledIf"))   rec.HasEnabledIf = v->AsBoolOr(false);
				if (const Json::Value* v = item.FindMember("enabledIfAny"))   rec.EnabledIfAny = v->AsBoolOr(false);
				if (const Json::Value* v = item.FindMember("enabledIfField")) rec.EnabledIfField = v->AsStringOr();
				if (const Json::Value* v = item.FindMember("enabledIfValue")) rec.EnabledIfValue = v->AsStringOr();

				if (rec.TypeTag.rfind("component:", 0) == 0) {
					rec.ComponentTypeName = rec.TypeTag.substr(10);
				}

				const bool isEnumField = (rec.TypeTag == "enum" || rec.TypeTag == "flagenum");
				const bool isEnumItem  = (rec.ItemTypeTag == "enum" || rec.ItemTypeTag == "flagenum");
				if (isEnumField || isEnumItem) {
					rec.Enum = std::make_shared<EnumDescriptor>();
					rec.Enum->IsFlags = (rec.TypeTag == "flagenum" || rec.ItemTypeTag == "flagenum");
					if (const Json::Value* flagsValue = item.FindMember("enumIsFlags")) {
						rec.Enum->IsFlags = flagsValue->AsBoolOr(rec.Enum->IsFlags);
					}
					if (const Json::Value* opts = item.FindMember("enumOptions"); opts && opts->IsArray()) {
						for (const Json::Value& opt : opts->GetArray()) {
							if (!opt.IsObject()) continue;
							EnumOption option;
							if (const Json::Value* name = opt.FindMember("name"))   option.Name = name->AsStringOr();
							if (const Json::Value* value = opt.FindMember("value")) option.Value = static_cast<int64_t>(value->AsDoubleOr(0.0));
							rec.Enum->Options.push_back(std::move(option));
						}
					}
				}

				if (rec.DisplayName.empty()) rec.DisplayName = rec.Name;
				fields.push_back(std::move(rec));
			}
			return fields;
		}

		// One-shot: apply the new value of a script field to one entity. Used
		// inside the descriptor's Set lambda; runs once per entity in the
		// PropertyDrawer's selection span.
		void ApplyScriptFieldEdit(const Entity& entity, std::size_t scriptIndex,
			const std::string& className, const std::string& fieldName,
			const std::string& newValueStr, bool isPlaying)
		{
			if (!entity.HasComponent<ScriptComponent>()) return;
			auto& sc = const_cast<Entity&>(entity).GetComponent<ScriptComponent>();
			if (scriptIndex >= sc.Scripts.size()) return;
			const auto& inst = sc.Scripts[scriptIndex];
			if (inst.GetClassName() != className) return;

			auto& callbacks = ScriptEngine::GetCallbacks();
			if (inst.HasManagedInstance() && callbacks.SetScriptField) {
				callbacks.SetScriptField(static_cast<int32_t>(inst.GetGCHandle()),
					fieldName.c_str(), newValueStr.c_str());
			}
			if (!isPlaying) {
				sc.PendingFieldValues[className + "." + fieldName] = newValueStr;
			}
		}

		void ApplyManagedComponentFieldEdit(const Entity& entity,
			const std::string& className, const std::string& fieldName,
			const std::string& newValueStr)
		{
			if (!entity.HasComponent<ScriptComponent>()) return;
			auto& sc = const_cast<Entity&>(entity).GetComponent<ScriptComponent>();
			sc.PendingFieldValues[className + "." + fieldName] = newValueStr;
		}

		// Per-entity, per-field value cache shared by every descriptor's Get lambda so multi-select sees real per-entity values.
		using ScriptFieldValueMap = std::unordered_map<uint32_t, std::unordered_map<std::string, std::string>>;

		// Returns the per-entity value for fieldName, falling back to defaultValue when the entity's slot doesn't match.
		PropertyValue LookupPerEntityValue(const std::shared_ptr<ScriptFieldValueMap>& perEntity,
			const Entity& entity, const std::string& fieldName,
			const std::string& defaultValue, PropertyType type)
		{
			if (perEntity) {
				auto eIt = perEntity->find(static_cast<uint32_t>(entity.GetHandle()));
				if (eIt != perEntity->end()) {
					auto fIt = eIt->second.find(fieldName);
					if (fIt != eIt->second.end()) {
						return PropertyValue::FromString(type, fIt->second);
					}
				}
			}
			return PropertyValue::FromString(type, defaultValue);
		}

		// "Truthy" check used by EnabledIf when the attribute didn't carry
		// an expected value. Mirrors the C# definition: bool/integer != 0,
		// non-empty string, non-empty list. Floats use exact != 0.
		bool IsValueTruthy(const std::string& valueStr, PropertyType type) {
			if (valueStr.empty()) return false;
			switch (type) {
			case PropertyType::Bool:
				return valueStr == "true" || valueStr == "True" || valueStr == "1";
			case PropertyType::Int8:  case PropertyType::Int16:
			case PropertyType::Int32: case PropertyType::Int64:
			case PropertyType::Enum:  case PropertyType::FlagEnum: {
				const PropertyValue parsed = PropertyValue::FromString(type, valueStr);
				return parsed.IntValue != 0;
			}
			case PropertyType::UInt8:  case PropertyType::UInt16:
			case PropertyType::UInt32: case PropertyType::UInt64: {
				const PropertyValue parsed = PropertyValue::FromString(type, valueStr);
				return parsed.UIntValue != 0;
			}
			case PropertyType::Float: case PropertyType::Double: {
				const PropertyValue parsed = PropertyValue::FromString(type, valueStr);
				return parsed.FloatValue != 0.0;
			}
			default:
				return !valueStr.empty();
			}
		}

		// Returns None (= always enabled) when the field isn't found, so a typo in [EnabledIf] doesn't permanently lock the row.
		PropertyType FindFieldType(const std::vector<EditorFieldRecord>& records, const std::string& name) {
			for (const auto& r : records) {
				if (r.Name == name) {
					if (r.TypeTag.rfind("component:", 0) == 0) return PropertyType::ComponentRef;
					if (r.TypeTag == "entity") return PropertyType::EntityRef;
					return PropertyTypeFromString(r.TypeTag);
				}
			}
			return PropertyType::None;
		}

		// Build EnabledIfFn: reads gate-field values from the per-entity snapshot so multi-edit sees true mixed state.
		std::function<bool(const Entity&)> BuildScriptEnabledIfPredicate(
			const EditorFieldRecord& rec, const std::vector<EditorFieldRecord>& siblingRecords,
			std::shared_ptr<ScriptFieldValueMap> perEntityValues)
		{
			if (!rec.HasEnabledIf || rec.EnabledIfField.empty()) return {};
			const PropertyType gateType = FindFieldType(siblingRecords, rec.EnabledIfField);
			if (gateType == PropertyType::None) return {};

			std::string targetField = rec.EnabledIfField;
			std::string expectedValue = rec.EnabledIfValue;
			bool any = rec.EnabledIfAny;
			return [perEntityValues, targetField = std::move(targetField),
					expectedValue = std::move(expectedValue), any, gateType]
				(const Entity& e) -> bool {
				if (!perEntityValues) return true;
				auto eIt = perEntityValues->find(static_cast<uint32_t>(e.GetHandle()));
				if (eIt == perEntityValues->end()) return true;
				auto fIt = eIt->second.find(targetField);
				if (fIt == eIt->second.end()) return true;
				if (any) return IsValueTruthy(fIt->second, gateType);
				const PropertyValue current  = PropertyValue::FromString(gateType, fIt->second);
				const PropertyValue expected = PropertyValue::FromString(gateType, expectedValue);
				return current == expected;
			};
		}

		// ── List field codec ─────────────────────────────────────────
		// Script list fields ride the single-string wire as their items, each
		// prefixed by a '\n' marker with embedded backslash/newline escaped. The
		// leading marker makes an empty list ("") distinct from a one-element list
		// whose element is empty ("\n") — without it, "+ Add" then leaving a None
		// reference (or "") would collapse back to zero elements on the next
		// round-trip. Must match the C# producer (EncodeListString) byte-for-byte.
		// The item type lives in PropertyMetadata::ListItemType, so the codec needs
		// it explicitly (PropertyValue::FromString can't infer it).
		PropertyType ResolveListItemType(const std::string& itemTag,
			std::string& outComponentName, AssetKind& outAssetKind)
		{
			outComponentName.clear();
			outAssetKind = AssetKind::Unknown;
			if (itemTag.rfind("component:", 0) == 0) { outComponentName = itemTag.substr(10); return PropertyType::ComponentRef; }
			if (itemTag.rfind("dataasset:", 0) == 0) { outAssetKind = AssetKind::DataAsset; return PropertyType::AssetRef; }
			if (itemTag == "entity") return PropertyType::EntityRef;
			return PropertyTypeFromString(itemTag);
		}

		std::string EncodeScriptList(const std::vector<PropertyValue>& items) {
			std::string out;
			for (const PropertyValue& item : items) {
				out.push_back('\n');
				for (char c : item.ToString()) {
					if (c == '\\') out.append("\\\\");
					else if (c == '\n') out.append("\\n");
					else out.push_back(c);
				}
			}
			return out;
		}

		std::vector<PropertyValue> DecodeScriptList(const std::string& text, PropertyType itemType) {
			std::vector<PropertyValue> items;
			if (text.empty()) return items;
			// Non-empty payload always opens with the '\n' element marker; skip it.
			std::size_t start = (text[0] == '\n') ? 1 : 0;
			std::string current;
			for (std::size_t i = start; i < text.size(); ++i) {
				char c = text[i];
				if (c == '\\' && i + 1 < text.size()) {
					char next = text[i + 1];
					if (next == 'n')  { current.push_back('\n'); ++i; continue; }
					if (next == '\\') { current.push_back('\\'); ++i; continue; }
				}
				if (c == '\n') { items.push_back(PropertyValue::FromString(itemType, current)); current.clear(); continue; }
				current.push_back(c);
			}
			items.push_back(PropertyValue::FromString(itemType, current));
			return items;
		}

		PropertyValue MakeListPropertyValue(const std::string& encoded, PropertyType itemType) {
			PropertyValue v;
			v.Type = PropertyType::List;
			v.ListValue = DecodeScriptList(encoded, itemType);
			return v;
		}

		PropertyValue LookupPerEntityListValue(const std::shared_ptr<ScriptFieldValueMap>& perEntity,
			const Entity& entity, const std::string& fieldName,
			const std::string& defaultValue, PropertyType itemType)
		{
			std::string raw = defaultValue;
			if (perEntity) {
				auto eIt = perEntity->find(static_cast<uint32_t>(entity.GetHandle()));
				if (eIt != perEntity->end()) {
					auto fIt = eIt->second.find(fieldName);
					if (fIt != eIt->second.end()) raw = fIt->second;
				}
			}
			return MakeListPropertyValue(raw, itemType);
		}

		// When d is a list, resolve item metadata (ListItemType + per-item
		// component/asset filter) and return the item type; None otherwise.
		PropertyType ConfigureListItemMetadata(PropertyDescriptor& d, const EditorFieldRecord& rec) {
			if (d.Type != PropertyType::List) return PropertyType::None;
			std::string itemComp;
			AssetKind itemKind = AssetKind::Unknown;
			const PropertyType itemType = ResolveListItemType(rec.ItemTypeTag, itemComp, itemKind);
			d.Metadata.ListItemType = itemType;
			if (itemType == PropertyType::ComponentRef) d.Metadata.ComponentTypeName = itemComp;
			if (itemType == PropertyType::AssetRef)     d.Metadata.AssetKindFilter = itemKind;
			return itemType;
		}

		// Build a PropertyDescriptor for a [ShowInEditor] field; Get reads the per-entity snapshot, Set propagates through SetScriptField + PendingFieldValues.
		PropertyDescriptor BuildScriptFieldDescriptor(EditorFieldRecord rec,
			const std::string& className, std::size_t scriptIndex,
			bool multiEditEnabled, bool isPlaying,
			std::shared_ptr<ScriptFieldValueMap> perEntityValues,
			const std::vector<EditorFieldRecord>& siblingRecords)
		{
			PropertyDescriptor d;
			d.Name = rec.Name;
			d.DisplayName = rec.DisplayName;
			d.Type = ResolveScriptFieldType(rec);
			d.Metadata.ReadOnly = rec.ReadOnly;
			d.Metadata.Tooltip = rec.Tooltip;
			d.Metadata.HeaderContent = rec.HeaderContent;
			d.Metadata.HeaderSize = rec.HeaderSize;
			d.Metadata.HasSpace = rec.HasSpace;
			d.Metadata.SpaceHeight = rec.SpaceHeight;
			d.Metadata.HasClamp = rec.HasClamp;
			d.Metadata.ClampMin = rec.ClampMin;
			d.Metadata.ClampMax = rec.ClampMax;
			d.Metadata.Enum = rec.Enum;
			d.Metadata.ComponentTypeName = rec.ComponentTypeName;
			d.Metadata.AssetKindFilter = (rec.TypeTag.rfind("dataasset:", 0) == 0) ? AssetKind::DataAsset : AssetKind::Unknown;
			// Script TextureRefs carry only a UUID — slices can't round-trip, so keep them out of the picker.
			d.Metadata.SuppressTextureSlices = true;
			if (auto pred = BuildScriptEnabledIfPredicate(rec, siblingRecords, perEntityValues)) {
				d.Metadata.EnabledIfFn = std::move(pred);
			}

			if (const PropertyType itemType = ConfigureListItemMetadata(d, rec); d.Type == PropertyType::List) {
				d.Get = [perEntityValues, fieldName = rec.Name, defaultValue = rec.Value, itemType]
					(const Entity& e) -> PropertyValue {
						return LookupPerEntityListValue(perEntityValues, e, fieldName, defaultValue, itemType);
					};
				d.Set = [className, fieldName = rec.Name, scriptIndex, isPlaying]
					(Entity& entity, const PropertyValue& v) {
						ApplyScriptFieldEdit(entity, scriptIndex, className, fieldName, EncodeScriptList(v.ListValue), isPlaying);
					};
				return d;
			}

			d.Get = [perEntityValues, fieldName = rec.Name, defaultValue = rec.Value, type = d.Type]
				(const Entity& e) -> PropertyValue {
					return LookupPerEntityValue(perEntityValues, e, fieldName, defaultValue, type);
				};

			d.Set = [className, fieldName = rec.Name, scriptIndex, multiEditEnabled, isPlaying]
				(Entity& entity, const PropertyValue& v) {
					const std::string newValueStr = v.ToString();
					ApplyScriptFieldEdit(entity, scriptIndex, className, fieldName, newValueStr, isPlaying);
					(void)multiEditEnabled; // PropertyDrawer already iterates the span
				};
			return d;
		}

		PropertyDescriptor BuildManagedComponentFieldDescriptor(EditorFieldRecord rec,
			const std::string& className,
			std::shared_ptr<ScriptFieldValueMap> perEntityValues,
			const std::vector<EditorFieldRecord>& siblingRecords)
		{
			PropertyDescriptor d;
			d.Name = rec.Name;
			d.DisplayName = rec.DisplayName;
			d.Type = ResolveScriptFieldType(rec);
			d.Metadata.ReadOnly = rec.ReadOnly;
			d.Metadata.Tooltip = rec.Tooltip;
			d.Metadata.HeaderContent = rec.HeaderContent;
			d.Metadata.HeaderSize = rec.HeaderSize;
			d.Metadata.HasSpace = rec.HasSpace;
			d.Metadata.SpaceHeight = rec.SpaceHeight;
			d.Metadata.HasClamp = rec.HasClamp;
			d.Metadata.ClampMin = rec.ClampMin;
			d.Metadata.ClampMax = rec.ClampMax;
			d.Metadata.Enum = rec.Enum;
			d.Metadata.ComponentTypeName = rec.ComponentTypeName;
			d.Metadata.AssetKindFilter = (rec.TypeTag.rfind("dataasset:", 0) == 0) ? AssetKind::DataAsset : AssetKind::Unknown;
			// Script TextureRefs carry only a UUID — slices can't round-trip, so keep them out of the picker.
			d.Metadata.SuppressTextureSlices = true;
			if (auto pred = BuildScriptEnabledIfPredicate(rec, siblingRecords, perEntityValues)) {
				d.Metadata.EnabledIfFn = std::move(pred);
			}

			if (const PropertyType itemType = ConfigureListItemMetadata(d, rec); d.Type == PropertyType::List) {
				d.Get = [perEntityValues, fieldName = rec.Name, defaultValue = rec.Value, itemType]
					(const Entity& e) -> PropertyValue {
						return LookupPerEntityListValue(perEntityValues, e, fieldName, defaultValue, itemType);
					};
				d.Set = [className, fieldName = rec.Name](Entity& entity, const PropertyValue& v) {
					ApplyManagedComponentFieldEdit(entity, className, fieldName, EncodeScriptList(v.ListValue));
				};
				return d;
			}

			d.Get = [perEntityValues, fieldName = rec.Name, defaultValue = rec.Value, type = d.Type]
				(const Entity& e) -> PropertyValue {
					return LookupPerEntityValue(perEntityValues, e, fieldName, defaultValue, type);
				};

			d.Set = [className, fieldName = rec.Name](Entity& entity, const PropertyValue& v) {
				ApplyManagedComponentFieldEdit(entity, className, fieldName, v.ToString());
			};
			return d;
		}

		std::string MakeFieldKey(std::size_t scriptIndex, const std::string& className,
			const std::string& fieldName)
		{
			return className + "#" + std::to_string(scriptIndex) + "." + fieldName;
		}

		// ── Nested struct/class grouping ─────────────────────────────
		// C# flattens a plain struct/class field into dotted records
		// ("P" -> "P.X", "P.Y"). Rebuild that shape as collapsible groups;
		// leaves still draw through the normal PropertyDrawer path.
		struct FieldTreeNode {
			std::string Label;
			int RecordIndex = -1;   // >= 0 => leaf record index
			std::vector<FieldTreeNode> Children;

			FieldTreeNode* FindGroupChild(const std::string& label) {
				for (auto& c : Children) {
					if (c.RecordIndex < 0 && c.Label == label) return &c;
				}
				return nullptr;
			}
		};

		void InsertFieldPath(FieldTreeNode& root, const std::string& name, int recordIndex) {
			FieldTreeNode* cur = &root;
			std::size_t start = 0;
			while (true) {
				const std::size_t dot = name.find('.', start);
				if (dot == std::string::npos) {
					FieldTreeNode leaf;
					leaf.Label = name.substr(start);
					leaf.RecordIndex = recordIndex;
					cur->Children.push_back(std::move(leaf));
					return;
				}
				const std::string seg = name.substr(start, dot - start);
				FieldTreeNode* child = cur->FindGroupChild(seg);
				if (!child) {
					FieldTreeNode group;
					group.Label = seg;
					cur->Children.push_back(std::move(group));
					child = &cur->Children.back();
				}
				cur = child;
				start = dot + 1;
			}
		}

		template <typename DrawLeafFn>
		void RenderFieldTree(const FieldTreeNode& node, const std::vector<EditorFieldRecord>& records,
			const std::string& idPrefix, DrawLeafFn& drawLeaf)
		{
			for (const auto& child : node.Children) {
				if (child.RecordIndex >= 0) {
					drawLeaf(records[static_cast<std::size_t>(child.RecordIndex)]);
					continue;
				}
				const std::string nodeId = idPrefix + "/" + child.Label;
				const std::string label = child.Label + "##grp_" + nodeId;
				// Framed so the fill goes through RenderFrame with the theme's
				// FrameRounding (rounded corners), matching component-section bars;
				// a plain TreeNode highlight is hardcoded to sharp corners.
				if (ImGui::TreeNodeEx(label.c_str(),
					ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_SpanAvailWidth)) {
					RenderFieldTree(child, records, nodeId, drawLeaf);
					ImGui::TreePop();
				}
			}
		}

		// Draws records as a nested tree when any field name is dotted, else a
		// flat list. drawLeaf renders one row via PropertyDrawer.
		template <typename DrawLeafFn>
		void RenderRecordsGrouped(const std::vector<EditorFieldRecord>& records,
			const std::string& idPrefix, DrawLeafFn drawLeaf)
		{
			bool anyNested = false;
			for (const auto& r : records) {
				if (r.Name.find('.') != std::string::npos) { anyNested = true; break; }
			}
			if (!anyNested) {
				for (const auto& rec : records) drawLeaf(rec);
				return;
			}
			FieldTreeNode root;
			for (int i = 0; i < static_cast<int>(records.size()); ++i) {
				InsertFieldPath(root, records[static_cast<std::size_t>(i)].Name, i);
			}
			RenderFieldTree(root, records, idPrefix, drawLeaf);
		}

		void RenderScriptFieldsForInstance(ScriptComponent& sc,
			const ScriptInstance& instance, std::size_t scriptIndex,
			std::span<const Entity> entities, bool multiEditEnabled)
		{
			auto& callbacks = ScriptEngine::GetCallbacks();
			const bool isPlaying = Application::GetIsPlaying();
			const bool hasLiveInstance = instance.HasManagedInstance();
			const std::string& className = instance.GetClassName();

			const char* json = nullptr;
			if (hasLiveInstance && callbacks.GetScriptFields) {
				json = callbacks.GetScriptFields(static_cast<int32_t>(instance.GetGCHandle()));
			}
			else if (callbacks.GetClassFieldDefs) {
				json = callbacks.GetClassFieldDefs(className.c_str());
			}
			if (!json || !*json || (json[0] == '[' && json[1] == ']')) return;

			auto records = ParseEditorFields(json);
			if (records.empty()) return;

			// Patch primary's reference records with primary's PendingFieldValues
			// so the class-default fallback used for entities-without-snapshots
			// matches what the user authored on the primary in editor mode.
			if (!hasLiveInstance) {
				for (auto& rec : records) {
					const std::string key = className + "." + rec.Name;
					auto it = sc.PendingFieldValues.find(key);
					if (it != sc.PendingFieldValues.end()) rec.Value = it->second;
				}
			}

			// Per-entity snapshot — GetScriptFields returns a static buffer, so parse each entity fully before moving on.
			auto perEntityValues = std::make_shared<ScriptFieldValueMap>();
			for (const Entity& e : entities) {
				if (!e.HasComponent<ScriptComponent>()) continue;
				const auto& eSc = e.GetComponent<ScriptComponent>();
				if (scriptIndex >= eSc.Scripts.size()) continue;
				const auto& eInst = eSc.Scripts[scriptIndex];
				if (eInst.GetClassName() != className) continue;

				auto& bucket = (*perEntityValues)[static_cast<uint32_t>(e.GetHandle())];
				if (eInst.HasManagedInstance() && callbacks.GetScriptFields) {
					const char* eJson = callbacks.GetScriptFields(static_cast<int32_t>(eInst.GetGCHandle()));
					if (eJson && *eJson && !(eJson[0] == '[' && eJson[1] == ']')) {
						auto eRecords = ParseEditorFields(eJson);
						for (auto& r : eRecords) {
							bucket[r.Name] = std::move(r.Value);
						}
					}
				}
				else {
					for (const auto& r : records) {
						const std::string key = className + "." + r.Name;
						auto it = eSc.PendingFieldValues.find(key);
						bucket[r.Name] = (it != eSc.PendingFieldValues.end()) ? it->second : r.Value;
					}
				}
			}

			ImGui::Indent(8.0f);
			RenderRecordsGrouped(records, MakeFieldKey(scriptIndex, className, ""),
				[&](const EditorFieldRecord& rec) {
					const std::string fieldKey = MakeFieldKey(scriptIndex, className, rec.Name);
					PropertyDescriptor descriptor = BuildScriptFieldDescriptor(
						rec, className, scriptIndex, multiEditEnabled, isPlaying, perEntityValues, records);
					PropertyDrawer::Draw(entities, descriptor, fieldKey);
				});
			ImGui::Unindent(8.0f);
		}

		// SceneSystem fields are scene-scoped: Set writes through SetSceneSystemFieldValue + SetSceneSystemField, ignoring the entity argument.
		PropertyDescriptor BuildSceneSystemFieldDescriptor(EditorFieldRecord rec,
			Scene* scene, const std::string& className)
		{
			PropertyDescriptor d;
			d.Name = rec.Name;
			d.DisplayName = rec.DisplayName;
			d.Type = ResolveScriptFieldType(rec);
			d.Metadata.ReadOnly = rec.ReadOnly;
			d.Metadata.Tooltip = rec.Tooltip;
			d.Metadata.HeaderContent = rec.HeaderContent;
			d.Metadata.HeaderSize = rec.HeaderSize;
			d.Metadata.HasSpace = rec.HasSpace;
			d.Metadata.SpaceHeight = rec.SpaceHeight;
			d.Metadata.HasClamp = rec.HasClamp;
			d.Metadata.ClampMin = rec.ClampMin;
			d.Metadata.ClampMax = rec.ClampMax;
			d.Metadata.Enum = rec.Enum;
			d.Metadata.ComponentTypeName = rec.ComponentTypeName;
			d.Metadata.AssetKindFilter = (rec.TypeTag.rfind("dataasset:", 0) == 0) ? AssetKind::DataAsset : AssetKind::Unknown;
			// Script TextureRefs carry only a UUID — slices can't round-trip, so keep them out of the picker.
			d.Metadata.SuppressTextureSlices = true;

			if (const PropertyType itemType = ConfigureListItemMetadata(d, rec); d.Type == PropertyType::List) {
				d.Get = [value = rec.Value, itemType](const Entity&) -> PropertyValue {
					return MakeListPropertyValue(value, itemType);
				};
				d.Set = [scene, className, fieldName = rec.Name](Entity&, const PropertyValue& v) {
					if (!scene) return;
					const std::string s = EncodeScriptList(v.ListValue);
					scene->SetSceneSystemFieldValue(className, fieldName, s);
					uint32_t handle = scene->GetSceneSystemHandle(className);
					if (handle != 0) {
						ScriptEngine::SetSceneSystemField(handle, fieldName.c_str(), s.c_str());
					}
				};
				return d;
			}

			d.Get = [value = rec.Value, type = d.Type](const Entity&) -> PropertyValue {
				return PropertyValue::FromString(type, value);
			};

			d.Set = [scene, className, fieldName = rec.Name](Entity&, const PropertyValue& v) {
				if (!scene) return;
				const std::string newValueStr = v.ToString();
				scene->SetSceneSystemFieldValue(className, fieldName, newValueStr);
				uint32_t handle = scene->GetSceneSystemHandle(className);
				if (handle != 0) {
					ScriptEngine::SetSceneSystemField(handle, fieldName.c_str(), newValueStr.c_str());
				}
			};
			return d;
		}

		// DataAsset fields target a standalone asset keyed by GUID: Get reads the
		// record value, Set writes through DataAssetManager (live instance + file).
		// The Entity argument is unused.
		PropertyDescriptor BuildDataAssetFieldDescriptor(EditorFieldRecord rec, uint64_t guid)
		{
			PropertyDescriptor d;
			d.Name = rec.Name;
			d.DisplayName = rec.DisplayName;
			d.Type = ResolveScriptFieldType(rec);
			d.Metadata.ReadOnly = rec.ReadOnly;
			d.Metadata.Tooltip = rec.Tooltip;
			d.Metadata.HeaderContent = rec.HeaderContent;
			d.Metadata.HeaderSize = rec.HeaderSize;
			d.Metadata.HasSpace = rec.HasSpace;
			d.Metadata.SpaceHeight = rec.SpaceHeight;
			d.Metadata.HasClamp = rec.HasClamp;
			d.Metadata.ClampMin = rec.ClampMin;
			d.Metadata.ClampMax = rec.ClampMax;
			d.Metadata.Enum = rec.Enum;
			d.Metadata.ComponentTypeName = rec.ComponentTypeName;
			d.Metadata.AssetKindFilter = (rec.TypeTag.rfind("dataasset:", 0) == 0) ? AssetKind::DataAsset : AssetKind::Unknown;
			// Script TextureRefs carry only a UUID — slices can't round-trip, so keep them out of the picker.
			d.Metadata.SuppressTextureSlices = true;

			if (const PropertyType itemType = ConfigureListItemMetadata(d, rec); d.Type == PropertyType::List) {
				d.Get = [value = rec.Value, itemType](const Entity&) -> PropertyValue {
					return MakeListPropertyValue(value, itemType);
				};
				d.Set = [guid, fieldName = rec.Name](Entity&, const PropertyValue& v) {
					DataAssetManager::SetField(guid, fieldName, EncodeScriptList(v.ListValue));
					DataAssetManager::Save(guid);
				};
				return d;
			}

			d.Get = [value = rec.Value, type = d.Type](const Entity&) -> PropertyValue {
				return PropertyValue::FromString(type, value);
			};

			d.Set = [guid, fieldName = rec.Name](Entity&, const PropertyValue& v) {
				const std::string newValueStr = v.ToString();
				DataAssetManager::SetField(guid, fieldName, newValueStr);
				DataAssetManager::Save(guid);
			};
			return d;
		}

		void RenderFieldsForManagedComponent(ScriptComponent& sc,
			const std::string& className, std::size_t componentIndex,
			std::span<const Entity> entities, bool /*multiEditEnabled*/)
		{
			auto& callbacks = ScriptEngine::GetCallbacks();
			if (!callbacks.GetClassFieldDefs) return;

			const char* json = callbacks.GetClassFieldDefs(className.c_str());
			if (!json || !*json || (json[0] == '[' && json[1] == ']')) return;

			auto records = ParseEditorFields(json);
			if (records.empty()) return;

			// Patch primary's reference records with primary's PendingFieldValues
			// so the descriptor's class-default fallback matches the primary
			// entity's authored value.
			for (auto& rec : records) {
				const std::string key = className + "." + rec.Name;
				auto it = sc.PendingFieldValues.find(key);
				if (it != sc.PendingFieldValues.end()) rec.Value = it->second;
			}

			// No live managed instance in edit mode — build the per-entity snapshot from PendingFieldValues.
			auto perEntityValues = std::make_shared<ScriptFieldValueMap>();
			for (const Entity& e : entities) {
				if (!e.HasComponent<ScriptComponent>()) continue;
				const auto& eSc = e.GetComponent<ScriptComponent>();
				if (!eSc.HasManagedComponent(className)) continue;

				auto& bucket = (*perEntityValues)[static_cast<uint32_t>(e.GetHandle())];
				for (const auto& r : records) {
					const std::string key = className + "." + r.Name;
					auto it = eSc.PendingFieldValues.find(key);
					bucket[r.Name] = (it != eSc.PendingFieldValues.end()) ? it->second : r.Value;
				}
			}

			ImGui::Indent(8.0f);
			const std::string idPrefix = className + "#component" + std::to_string(componentIndex);
			RenderRecordsGrouped(records, idPrefix, [&](const EditorFieldRecord& rec) {
				const std::string fieldKey = idPrefix + "." + rec.Name;
				PropertyDescriptor descriptor = BuildManagedComponentFieldDescriptor(rec, className, perEntityValues, records);
				PropertyDrawer::Draw(entities, descriptor, fieldKey);
			});
			ImGui::Unindent(8.0f);
		}

	} // namespace

	namespace {
		using ScriptPickerEntry = EditorScriptDiscovery::ScriptEntry;

		// Grouped so Shutdown() can reset all picker state in one assignment.
		struct ScriptPickerState {
			bool Open = false;
			char Search[128] = {};
			std::vector<ScriptPickerEntry> Entries;
			EntityHandle TargetEntity = entt::null;
		};
		ScriptPickerState s_PickerState;

		void OpenScriptPicker(EntityHandle entity) {
			s_PickerState.Open = true;
			s_PickerState.Search[0] = '\0';
			s_PickerState.TargetEntity = entity;
			s_PickerState.Entries.clear();
			EditorScriptDiscovery::CollectProjectScriptEntries(s_PickerState.Entries);
		}

		void RenderScriptPicker() {
			if (!s_PickerState.Open) return;

			ImGui::SetNextWindowSize(ImVec2(320, 380), ImGuiCond_FirstUseEver);
			ImGuiImplWebGPU::SetNextWindowAsNativeDialog();
			if (!ImGui::Begin("Select Script", &s_PickerState.Open)) {
				ImGui::End();
				return;
			}

			const float closeButtonSize = ImGui::GetFrameHeight();
			const float searchWidth = std::max(ImGui::GetContentRegionAvail().x - closeButtonSize - ImGui::GetStyle().ItemSpacing.x, 1.0f);
			ImGui::SetNextItemWidth(searchWidth);
			ImGui::InputTextWithHint("##ScriptSearch", "Search...", s_PickerState.Search, sizeof(s_PickerState.Search));
			ImGui::SameLine();
			if (ImGui::Button("X##ScriptPickerClose", ImVec2(closeButtonSize, closeButtonSize))) {
				s_PickerState.Open = false;
			}
			ImGui::Separator();

			ImGui::BeginChild("##ScriptList");

			std::string filter = ToLowerCopy(std::string(s_PickerState.Search));

			for (const auto& entry : s_PickerState.Entries) {
				if (!filter.empty()) {
					std::string lowerName = ToLowerCopy(entry.ClassName);
					std::string lowerPath = ToLowerCopy(entry.Path.string());
					if (lowerName.find(filter) == std::string::npos && lowerPath.find(filter) == std::string::npos) continue;
				}

				bool truncated = false;
				const std::string displayLabel = ImGuiUtils::Ellipsize(
					entry.ClassName + "  " + entry.Extension,
					ImGui::GetContentRegionAvail().x,
					&truncated);
				std::string label = displayLabel + "##" + entry.Path.string();
				if (ImGui::Selectable(label.c_str(), false)) {
					Scene* scene = ScriptEngine::GetScene();
					if (!scene) scene = SceneManager::Get().GetActiveScene();
					if (scene && scene->IsValid(s_PickerState.TargetEntity)
						&& scene->HasComponent<ScriptComponent>(s_PickerState.TargetEntity)) {
						auto& sc = scene->GetComponent<ScriptComponent>(s_PickerState.TargetEntity);
						if (!sc.HasScript(entry.ClassName, entry.Type)) {
							sc.AddScript(entry.ClassName, entry.Type);
							scene->MarkDirty();
						}
					}
					s_PickerState.Open = false;
				}

				if (ImGui::IsItemHovered() && (truncated || !entry.Path.empty())) {
					const std::string path = Path::ToProjectRelativeDisplay(entry.Path.string());
					ImGui::SetTooltip("%s", path.c_str());
				}
			}

			ImGui::EndChild();
			ImGui::End();
		}

		// Shape must match across the selection: mismatched script index `i` refers to different scripts on different entities.
		bool ScriptComponentShapeMatches(const ScriptComponent& a, const ScriptComponent& b) {
			if (a.ManagedComponents.size() != b.ManagedComponents.size()) return false;
			if (a.Scripts.size() != b.Scripts.size()) return false;
			for (std::size_t i = 0; i < a.ManagedComponents.size(); ++i) {
				if (a.ManagedComponents[i] != b.ManagedComponents[i]) return false;
			}
			for (std::size_t i = 0; i < a.Scripts.size(); ++i) {
				if (a.Scripts[i].GetClassName() != b.Scripts[i].GetClassName()) return false;
				if (a.Scripts[i].GetType() != b.Scripts[i].GetType()) return false;
			}
			return true;
		}
	}

	void DrawScriptComponentInspector(std::span<const Entity> entities)
	{
		if (entities.empty()) return;

		// Selection may not uniformly carry ScriptComponent (mid-edit add/remove, stale undo); bail rather than trip EnTT's assertion.
		const Entity& primary = entities[0];
		if (!primary.HasComponent<ScriptComponent>()) {
			return;
		}
		auto& scriptComp = const_cast<Entity&>(primary).GetComponent<ScriptComponent>();
		bool shapeUniform = true;
		for (std::size_t i = 1; i < entities.size(); ++i) {
			if (!entities[i].HasComponent<ScriptComponent>()) {
				shapeUniform = false;
				continue;
			}
			const auto& other = entities[i].GetComponent<ScriptComponent>();
			if (!ScriptComponentShapeMatches(scriptComp, other)) {
				shapeUniform = false;
				break;
			}
		}
		if (entities.size() > 1 && !shapeUniform) {
			ImGui::TextDisabled("Multi-edit limited - script set differs across selection");
		}
		const bool multiEditEnabled = shapeUniform;
		const std::span<const Entity> propagationSpan = multiEditEnabled
			? entities
			: std::span<const Entity>(&primary, 1);

		size_t removeIndex = SIZE_MAX;
		size_t removeManagedComponentIndex = SIZE_MAX;

		for (size_t i = 0; i < scriptComp.ManagedComponents.size(); i++) {
			const std::string& className = scriptComp.ManagedComponents[i];
			std::string label = className.empty() ? "C# Component" : className + " (C# Component)";

			ImGui::PushID(("managed-component-" + std::to_string(i)).c_str());

			bool removeRequested = false;
			bool open = ImGuiUtils::BeginComponentSection(label.c_str(), removeRequested);
			if (removeRequested) removeManagedComponentIndex = i;

			if (open) {
				DrawOpenScriptButton(className,
					[](const EditorScriptDiscovery::ScriptEntry& e) {
						return e.Type == ScriptType::Managed;
					},
					className);

				if (ScriptEngine::IsInitialized()) {
					RenderFieldsForManagedComponent(scriptComp, className, i, propagationSpan, multiEditEnabled);
				}
				else {
					ImGui::TextDisabled("Script engine not initialized");
				}
				ImGuiUtils::EndComponentSection();
			}
			ImGui::PopID();
		}

		for (size_t i = 0; i < scriptComp.Scripts.size(); i++) {
			auto& instance = scriptComp.Scripts[i];
			// Distinguish EntityScript ("C# Script") from :IComponent struct ("C# Native Component") by checking ComponentRegistry::isDynamic.
			std::string label;
			if (instance.GetClassName().empty()) {
				label = "Script";
			}
			else {
				const ComponentInfo* dynamicInfo = SceneManager::Get().GetComponentRegistry()
					.FindBySerializedName(instance.GetClassName());
				const bool isNativeComponent = dynamicInfo && dynamicInfo->isDynamic;
				label = instance.GetClassName()
					+ (isNativeComponent ? " (C# Native Component)" : " (C# Script)");
			}

			ImGui::PushID(static_cast<int>(i));

			bool removeRequested = false;
			bool open = ImGuiUtils::BeginComponentSection(label.c_str(), removeRequested);
			if (removeRequested) removeIndex = i;

			if (open) {
				const ScriptType instanceType = instance.GetType();
				DrawOpenScriptButton(instance.GetClassName(),
					[instanceType](const EditorScriptDiscovery::ScriptEntry& e) {
						return e.Type == instanceType;
					},
					label);

				if (instance.GetType() != ScriptType::Native && ScriptEngine::IsInitialized()) {
					RenderScriptFieldsForInstance(scriptComp, instance, i, propagationSpan, multiEditEnabled);
				}
				ImGuiUtils::EndComponentSection();
			}
			ImGui::PopID();
		}

		if (removeIndex != SIZE_MAX) {
			if (multiEditEnabled) {
				for (const Entity& e : entities) {
					ScriptSystem::RemoveScript(const_cast<Entity&>(e), removeIndex);
				}
			}
			else {
				ScriptSystem::RemoveScript(const_cast<Entity&>(primary), removeIndex);
			}
		}
		if (removeManagedComponentIndex != SIZE_MAX && removeManagedComponentIndex < scriptComp.ManagedComponents.size()) {
			const std::string className = scriptComp.ManagedComponents[removeManagedComponentIndex];
			if (multiEditEnabled) {
				for (const Entity& e : entities) {
					auto& sc = const_cast<Entity&>(e).GetComponent<ScriptComponent>();
					if (sc.RemoveManagedComponent(className)) {
						if (Scene* scene = const_cast<Entity&>(e).GetScene()) scene->MarkDirty();
					}
				}
			}
			else {
				if (scriptComp.RemoveManagedComponent(className)) {
					if (Scene* scene = const_cast<Entity&>(primary).GetScene()) scene->MarkDirty();
				}
			}
		}

		RenderScriptPicker();
		// NOTE: ReferencePicker::RenderPopup() intentionally NOT called here — the outer loop already renders it once; calling it again duplicates IDs and triggers ImGui's conflicting-ID assertion.
	}

	void DrawSceneSystemFields(Scene& scene, const std::string& className)
	{
		// Render unconditionally so it's visible even when the system has no fields or the engine isn't initialised.
		DrawOpenScriptButton(className,
			[](const EditorScriptDiscovery::ScriptEntry& e) {
				return e.IsSceneSystem;
			},
			className);

		if (!ScriptEngine::IsInitialized()) {
			ImGui::TextDisabled("Script engine not initialized");
			return;
		}

		auto& callbacks = ScriptEngine::GetCallbacks();
		const uint32_t handle = scene.GetSceneSystemHandle(className);
		const bool hasLiveInstance = (handle != 0);

		// Live instance: read actual managed state. Edit mode: class defaults patched with authored values below.
		const char* json = nullptr;
		if (hasLiveInstance) {
			json = ScriptEngine::GetSceneSystemFields(handle);
		}
		else if (callbacks.GetClassFieldDefs) {
			json = callbacks.GetClassFieldDefs(className.c_str());
		}
		if (!json || !*json || (json[0] == '[' && json[1] == ']')) return;

		auto records = ParseEditorFields(json);
		if (records.empty()) return;

		// In edit mode: overlay scene's authored values over class defaults so the inspector reflects what will be flushed at Awake.
		if (!hasLiveInstance) {
			if (const auto* overrides = scene.GetSceneSystemFieldValues(className)) {
				for (auto& rec : records) {
					auto it = overrides->find(rec.Name);
					if (it != overrides->end()) rec.Value = it->second;
				}
			}
		}

		// Placeholder entity carries the Scene* so MarkSceneDirty fires; MakeScenePlaceholder avoids Scene::GetEntity which asserts on null handles.
		const Entity placeholder = Entity::MakeScenePlaceholder(scene);
		const std::span<const Entity> entitySpan(&placeholder, 1);

		const std::string idPrefix = "gamesystem:" + className;
		RenderRecordsGrouped(records, idPrefix, [&](const EditorFieldRecord& rec) {
			const std::string fieldKey = idPrefix + "." + rec.Name;
			PropertyDescriptor descriptor = BuildSceneSystemFieldDescriptor(rec, &scene, className);
			PropertyDrawer::Draw(entitySpan, descriptor, fieldKey);
		});
	}

	void DrawDataAssetFields(uint64_t assetGuid)
	{
		if (!ScriptEngine::IsInitialized()) {
			ImGui::TextDisabled("Script engine not initialized");
			return;
		}
		if (assetGuid == 0) {
			ImGui::TextDisabled("Invalid data asset");
			return;
		}

		// GetFields ensures the managed instance is loaded, then returns its serialized fields.
		const char* json = DataAssetManager::GetFields(assetGuid);
		const std::string typeName = DataAssetManager::GetTypeName(assetGuid);

		if (!typeName.empty()) {
			ImGui::TextDisabled("Type:");
			ImGui::SameLine();
			ImGui::TextUnformatted(typeName.c_str());
			ImGui::Separator();
		}

		if (!json || !*json || (json[0] == '[' && json[1] == ']')) {
			ImGui::TextDisabled(typeName.empty()
				? "Data asset type not found (is the script assembly built?)"
				: "No editable fields");
			return;
		}

		auto records = ParseEditorFields(json);
		if (records.empty()) {
			ImGui::TextDisabled("No editable fields");
			return;
		}

		// No entity backs a data asset; the descriptors ignore the entity argument.
		const Entity placeholder = Entity::Null;
		const std::span<const Entity> entitySpan(&placeholder, 1);

		const std::string idPrefix = "dataasset:" + std::to_string(assetGuid);
		RenderRecordsGrouped(records, idPrefix, [&](const EditorFieldRecord& rec) {
			const std::string fieldKey = idPrefix + "." + rec.Name;
			PropertyDescriptor descriptor = BuildDataAssetFieldDescriptor(rec, assetGuid);
			PropertyDrawer::Draw(entitySpan, descriptor, fieldKey);
		});
	}

	namespace ScriptComponentInspector {
		// Reset all picker state on detach so project-switch / hot-reload starts clean.
		void Shutdown() {
			s_PickerState = ScriptPickerState{};
		}
	}

} // namespace Index
