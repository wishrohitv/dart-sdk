// Copyright (c) 2022, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'dart:math' show min;

import 'package:kernel/ast.dart';
import 'package:vm/metadata/procedure_attributes.dart';
import 'package:vm/metadata/table_selector.dart';
import 'package:wasm_builder/wasm_builder.dart' as w;

import 'class_info.dart';
import 'param_info.dart';
import 'reference_extensions.dart';
import 'translator.dart';

/// Information for a dispatch table selector.
///
/// A selector encapsulates information to generate code that selects the right
/// member (method, getter, setter) implementation in an instance invocation,
/// from the dispatch table. Dispatch table is generated by [DispatchTable].
///
/// Target of a selector is a method, getter, or setter [Reference]. A target
/// does not have to correspond to a user-written Dart member, it can be for a
/// generated one. For example, for torn-off methods, we generate a [Reference]
/// for the tear-off getter a selector for it.
class SelectorInfo {
  final Translator translator;

  /// Unique ID of the selector.
  final int id;

  /// Number of use sites of the selector.
  final int callCount;

  /// Least upper bound of [ParameterInfo]s of all targets.
  final ParameterInfo paramInfo;

  /// Is this an implicit or explicit setter?
  final bool isSetter;

  /// Does this method have any tear-off uses?
  bool hasTearOffUses = false;

  /// Does this method have any non-this uses?
  bool hasNonThisUses = false;

  /// Targets for all concrete classes implementing this selector.
  ///
  /// As a subclass hierarchy often inherits the same target, we associate the
  /// target with a range of class ids. The ranges are non-empty,
  /// non-overlapping and sorted in ascending order.
  late final List<({Range range, Reference target})> targetRanges;
  late final Set<Reference> targetSet =
      targetRanges.map((e) => e.target).toSet();
  late final List<({Range range, Reference target})> staticDispatchRanges;

  /// Wasm function type for the selector.
  ///
  /// This should be read after all targets have been added to the selector.
  late final w.FunctionType signature = _computeSignature();

  /// Number of concrete classes that provide this selector.
  late final int concreteClasses;

  /// Offset of the selector in the dispatch table.
  ///
  /// For a class in [targetRanges], `class ID + offset` gives the offset of the
  /// class member for this selector.
  int? offset;

  /// The selector's member's name.
  final String name;

  /// Whether the selector is for `Object.noSuchMethod`. We introduce instance
  /// invocations of `noSuchMethod` in dynamic calls, so we always add
  /// `noSuchMethod` overrides to the dispatch table.
  final bool isNoSuchMethod;

  SelectorInfo._(
      this.translator, this.id, this.name, this.callCount, this.paramInfo,
      {required this.isSetter, required this.isNoSuchMethod});

  /// Compute the signature for the functions implementing members targeted by
  /// this selector.
  ///
  /// When the selector has multiple targets, the type of each parameter/return
  /// is the upper bound across all targets, such that all targets have the
  /// same signature, and the actual representation types of the parameters and
  /// returns are subtypes (resp. supertypes) of the types in the signature.
  w.FunctionType _computeSignature() {
    var nameIndex = paramInfo.nameIndex;
    final int returnCount = isSetter ? 0 : 1;
    List<Set<w.ValueType>> inputSets =
        List.generate(1 + paramInfo.paramCount, (_) => {});
    List<Set<w.ValueType>> outputSets = List.generate(returnCount, (_) => {});
    List<bool> ensureBoxed = List.filled(1 + paramInfo.paramCount, false);
    for (final (range: _, :target) in targetRanges) {
      Member member = target.asMember;
      DartType receiver =
          InterfaceType(member.enclosingClass!, Nullability.nonNullable);
      List<DartType> positional;
      Map<String, DartType> named;
      List<DartType> returns;
      if (member is Field) {
        if (target.isImplicitGetter) {
          positional = const [];
          named = const {};
          returns = [translator.typeOfReturnValue(member)];
        } else {
          positional = [member.setterType];
          named = const {};
          returns = const [];
        }
      } else {
        FunctionNode function = member.function!;
        if (target.isTearOffReference) {
          positional = const [];
          named = const {};
          returns = [function.computeFunctionType(Nullability.nonNullable)];
        } else {
          final typeForParam = translator.typeOfParameterVariable;
          positional = [
            for (int i = 0; i < function.positionalParameters.length; i++)
              typeForParam(function.positionalParameters[i],
                  i < function.requiredParameterCount)
          ];
          named = {
            for (VariableDeclaration param in function.namedParameters)
              param.name!: typeForParam(param, param.isRequired)
          };
          returns = target.isSetter
              ? const []
              : [translator.typeOfReturnValue(member)];
        }
      }
      assert(returns.length <= outputSets.length);
      inputSets[0].add(translator.translateType(receiver));
      for (int i = 0; i < positional.length; i++) {
        DartType type = positional[i];
        inputSets[1 + i].add(translator.translateType(type));
        ensureBoxed[1 + i] |=
            paramInfo.positional[i] == ParameterInfo.defaultValueSentinel;
      }
      for (String name in named.keys) {
        int i = nameIndex[name]!;
        DartType type = named[name]!;
        inputSets[1 + i].add(translator.translateType(type));
        ensureBoxed[1 + i] |=
            paramInfo.named[name] == ParameterInfo.defaultValueSentinel;
      }
      for (int i = 0; i < returnCount; i++) {
        if (i < returns.length) {
          DartType type = returns[i];
          outputSets[i].add(translator.translateType(type));
        } else {
          outputSets[i].add(translator.topInfo.nullableType);
        }
      }
    }

    List<w.ValueType> typeParameters = List.filled(paramInfo.typeParamCount,
        translator.classInfo[translator.typeClass]!.nonNullableType);
    List<w.ValueType> inputs = List.generate(inputSets.length,
        (i) => _upperBound(inputSets[i], ensureBoxed: ensureBoxed[i]));
    if (name == '==') {
      // == can't be called with null
      inputs[1] = inputs[1].withNullability(false);
    }
    List<w.ValueType> outputs = List.generate(outputSets.length,
        (i) => _upperBound(outputSets[i], ensureBoxed: false));
    return translator.typesBuilder.defineFunction(
        [inputs[0], ...typeParameters, ...inputs.sublist(1)], outputs);
  }

  w.ValueType _upperBound(Set<w.ValueType> types, {required bool ensureBoxed}) {
    if (types.isEmpty) {
      // This happens if the selector doesn't have any targets. Any call site of
      // such a selector is unreachable. Though such call sites still have to
      // evaluate receiver and arguments. Doing so requires the signature. So we
      // create a dummy signature with top types.
      return translator.topInfo.nullableType;
    }
    if (!ensureBoxed && types.length == 1 && types.single.isPrimitive) {
      // Unboxed primitive.
      return types.single;
    }
    final bool nullable = types.any((type) => type.nullable);
    int minDepth = 999999999;
    Set<w.DefType> heapTypes = types
        .where((type) => type is! w.RefType || type.heapType is w.DefType)
        .map((type) {
      w.DefType def = type is w.RefType
          ? type.heapType as w.DefType
          : translator.classInfo[translator.boxedClasses[type]!]!.struct;
      minDepth = min(minDepth, def.depth);
      return def;
    }).toSet();
    if (heapTypes.isEmpty) {
      // Only abstract heap types.
      Set<w.HeapType> heapTypes =
          types.map((type) => (type as w.RefType).heapType).toSet();
      return w.RefType(heapTypes.single, nullable: nullable);
    }
    int targetDepth = minDepth;
    while (heapTypes.length > 1) {
      heapTypes = heapTypes.map((s) {
        while (s.depth > targetDepth) {
          s = s.superType!;
        }
        return s;
      }).toSet();
      targetDepth -= 1;
    }
    return w.RefType.def(heapTypes.single, nullable: nullable);
  }
}

/// Builds the dispatch table for member calls.
class DispatchTable {
  static const _functionType = w.RefType.func(nullable: true);

  final Translator translator;
  final List<TableSelectorInfo> _selectorMetadata;
  final Map<TreeNode, ProcedureAttributesMetadata> _procedureAttributeMetadata;

  /// Maps selector IDs to selectors.
  final Map<int, SelectorInfo> _selectorInfo = {};

  /// Maps member names to getter selectors with the same member name.
  final Map<String, Set<SelectorInfo>> _dynamicGetters = {};

  /// Maps member names to setter selectors with the same member name.
  final Map<String, Set<SelectorInfo>> _dynamicSetters = {};

  /// Maps member names to method selectors with the same member name.
  final Map<String, Set<SelectorInfo>> _dynamicMethods = {};

  /// Contents of [_definedWasmTable]. For a selector with ID S and a target
  /// class of the selector with ID C, `table[S + C]` gives the reference to the
  /// class member for the selector.
  late final List<Reference?> _table;

  late final w.TableBuilder _definedWasmTable;
  final WasmTableImporter _importedWasmTables;

  /// The Wasm table for the dispatch table.
  w.Table getWasmTable(w.ModuleBuilder module) =>
      _importedWasmTables.get(_definedWasmTable, module);

  DispatchTable(this.translator)
      : _selectorMetadata =
            (translator.component.metadata["vm.table-selector.metadata"]
                    as TableSelectorMetadataRepository)
                .mapping[translator.component]!
                .selectors,
        _procedureAttributeMetadata =
            (translator.component.metadata["vm.procedure-attributes.metadata"]
                    as ProcedureAttributesMetadataRepository)
                .mapping,
        _importedWasmTables = WasmTableImporter(translator, 'dispatch');

  SelectorInfo selectorForTarget(Reference target) {
    Member member = target.asMember;
    bool isGetter = target.isGetter || target.isTearOffReference;
    ProcedureAttributesMetadata metadata = _procedureAttributeMetadata[member]!;
    int selectorId = isGetter
        ? metadata.getterSelectorId
        : metadata.methodOrSetterSelectorId;
    return _selectorInfo[selectorId]!;
  }

  SelectorInfo _createSelectorForTarget(Reference target) {
    Member member = target.asMember;
    bool isGetter = target.isGetter || target.isTearOffReference;
    bool isSetter = target.isSetter;
    ProcedureAttributesMetadata metadata = _procedureAttributeMetadata[member]!;
    int selectorId = isGetter
        ? metadata.getterSelectorId
        : metadata.methodOrSetterSelectorId;
    ParameterInfo paramInfo = ParameterInfo.fromMember(target);

    // _WasmBase and its subclass methods cannot be called dynamically
    final cls = member.enclosingClass;
    final isWasmType = cls != null && translator.isWasmType(cls);

    final calledDynamically = !isWasmType &&
        (metadata.getterCalledDynamically ||
            metadata.methodOrSetterCalledDynamically ||
            member.name.text == "call");

    final selector = _selectorInfo.putIfAbsent(
        selectorId,
        () => SelectorInfo._(translator, selectorId, member.name.text,
            _selectorMetadata[selectorId].callCount, paramInfo,
            isSetter: isSetter,
            isNoSuchMethod: member == translator.objectNoSuchMethod));
    assert(selector.isSetter == isSetter);
    selector.hasTearOffUses |= metadata.hasTearOffUses;
    selector.hasNonThisUses |= metadata.hasNonThisUses;
    selector.paramInfo.merge(paramInfo);
    if (calledDynamically) {
      if (isGetter) {
        (_dynamicGetters[member.name.text] ??= {}).add(selector);
      } else if (isSetter) {
        (_dynamicSetters[member.name.text] ??= {}).add(selector);
      } else {
        (_dynamicMethods[member.name.text] ??= {}).add(selector);
      }
    }
    return selector;
  }

  /// Get selectors for getters and tear-offs with the given name.
  Iterable<SelectorInfo> dynamicGetterSelectors(String memberName) =>
      _dynamicGetters[memberName] ?? Iterable.empty();

  /// Get selectors for setters with the given name.
  Iterable<SelectorInfo> dynamicSetterSelectors(String memberName) =>
      _dynamicSetters[memberName] ?? Iterable.empty();

  /// Get selectors for methods with the given name.
  Iterable<SelectorInfo> dynamicMethodSelectors(String memberName) =>
      _dynamicMethods[memberName] ?? Iterable.empty();

  void build() {
    // Collect class/selector combinations

    // Maps class to selector IDs of the class
    final selectorsInClass = <Class, Map<SelectorInfo, Reference>>{};
    final staticDispatchPragmas = <Reference>{};

    // Add classes to selector targets for their members
    for (ClassInfo info in translator.classesSupersFirst) {
      final Class cls = info.cls ?? translator.coreTypes.objectClass;
      final Map<SelectorInfo, Reference> selectors;

      // Add the class to its inherited members' selectors. Skip `_WasmBase`:
      // it's defined as a Dart class (in `dart._wasm` library) but it's special
      // and does not inherit from `Object`.
      final ClassInfo? superInfo = info.superInfo;
      if (superInfo == null || cls == translator.wasmTypesBaseClass) {
        selectors = {};
      } else {
        final Class superCls =
            superInfo.cls ?? translator.coreTypes.objectClass;
        selectors = Map.of(selectorsInClass[superCls]!);
      }

      /// Add a method (or getter, setter) of the current class ([info]) to
      /// [reference]'s selector's targets.
      ///
      /// Because we visit a superclass before its subclasses, if the class
      /// inherits [reference], then the selector will already have a target
      /// for the class. Override that target if [reference] is a not abstract.
      /// If it's abstract, then the superclass's method will be called, so do
      /// not update the target.
      void addMember(Reference reference, bool staticDispatch) {
        SelectorInfo selector = _createSelectorForTarget(reference);
        if (reference.asMember.isAbstract) {
          // Reference is abstract, do not override inherited concrete member
          selectors[selector] ??= reference;
        } else {
          // Reference is concrete, override inherited member
          selectors[selector] = reference;

          if (staticDispatch) staticDispatchPragmas.add(reference);
        }
      }

      // Add the class to its non-static members' selectors. If `info.cls` is
      // `null`, that means [info] represents the `#Top` type, which is not a
      // Dart class but has the members of `Object`.
      for (Member member in cls.members) {
        // Skip static members
        if (!member.isInstanceMember) {
          continue;
        }
        final bool staticDispatch =
            translator.getPragma<bool>(member, 'wasm:static-dispatch', true) ??
                false;
        if (member is Field) {
          addMember(member.getterReference, staticDispatch);
          if (member.hasSetter) {
            addMember(member.setterReference!, staticDispatch);
          }
        } else if (member is Procedure) {
          addMember(member.reference, staticDispatch);
          // `hasTearOffUses` can be true for operators as well, even though
          // it's not possible to tear-off an operator. (no syntax for it)
          if (member.kind == ProcedureKind.Method &&
              _procedureAttributeMetadata[member]!.hasTearOffUses) {
            addMember(member.tearOffReference, staticDispatch);
          }
        }
      }
      selectorsInClass[cls] = selectors;
    }

    final selectorTargets = <SelectorInfo, Map<int, Reference>>{};
    final maxConcreteClassId = translator.classIdNumbering.maxConcreteClassId;
    for (int classId = 0; classId <= maxConcreteClassId; ++classId) {
      final cls = translator.classes[classId].cls;
      if (cls != null) {
        selectorsInClass[cls]!.forEach((selectorInfo, target) {
          if (!target.asMember.isAbstract) {
            selectorTargets.putIfAbsent(selectorInfo, () => {})[classId] =
                target;
          }
        });
      }
    }

    selectorTargets
        .forEach((SelectorInfo selector, Map<int, Reference> targets) {
      selector.concreteClasses = targets.length;

      final List<({Range range, Reference target})> ranges = targets.entries
          .map((entry) =>
              (range: Range(entry.key, entry.key), target: entry.value))
          .toList()
        ..sort((a, b) => a.range.start.compareTo(b.range.start));
      assert(ranges.isNotEmpty);
      int writeIndex = 0;
      for (int readIndex = 1; readIndex < ranges.length; ++readIndex) {
        final current = ranges[writeIndex];
        final next = ranges[readIndex];
        assert(next.range.length == 1);
        if ((current.range.end + 1) == next.range.start &&
            identical(current.target, next.target)) {
          ranges[writeIndex] = (
            range: Range(current.range.start, next.range.end),
            target: current.target
          );
        } else {
          ranges[++writeIndex] = next;
        }
      }
      ranges.length = writeIndex + 1;

      final staticDispatchRanges = (translator
                  .options.polymorphicSpecialization ||
              ranges.length == 1)
          ? ranges
          : ranges
              .where((range) => staticDispatchPragmas.contains(range.target))
              .toList();
      selector.targetRanges = ranges;
      selector.staticDispatchRanges = staticDispatchRanges;
    });

    _selectorInfo.forEach((_, selector) {
      if (!selectorTargets.containsKey(selector)) {
        selector.concreteClasses = 0;
        selector.targetRanges = [];
      }
    });

    // Assign selector offsets

    bool isUsedViaDispatchTableCall(SelectorInfo selector) {
      if (selector.isNoSuchMethod) {
        return true;
      }
      if (selector.callCount == 0) return false;
      if (selector.targetRanges.length <= 1) return false;
      if (selector.staticDispatchRanges.length ==
          selector.targetRanges.length) {
        return false;
      }
      return true;
    }

    final List<SelectorInfo> selectors =
        selectorTargets.keys.where(isUsedViaDispatchTableCall).toList();

    // Sort the selectors based on number of targets and number of use sites.
    // This is a heuristic to keep the table small.
    //
    // Place selectors with more targets first as they are less likely to fit
    // into the gaps left by selectors placed earlier.
    //
    // Among the selectors with approximately same number of targets, place
    // more used ones first, as the smaller selector offset will have a smaller
    // instruction encoding.
    int selectorSortWeight(SelectorInfo selector) =>
        selector.concreteClasses * 10 + selector.callCount;

    selectors.sort((a, b) => selectorSortWeight(b) - selectorSortWeight(a));

    final rows = <Row<Reference>>[];
    for (final selector in selectors) {
      final rowValues = <({int index, Reference value})>[];
      for (final (:range, :target) in selector.targetRanges) {
        for (int classId = range.start; classId <= range.end; ++classId) {
          rowValues.add((index: classId, value: target));
        }
      }
      rowValues.sort((a, b) => a.index.compareTo(b.index));
      rows.add(Row(rowValues));
    }

    _table = buildRowDisplacementTable<Reference>(rows);
    for (int i = 0; i < rows.length; ++i) {
      selectors[i].offset = rows[i].offset;
    }

    _definedWasmTable =
        translator.mainModule.tables.define(_functionType, _table.length);
    for (final module in translator.modules) {
      // Ensure the dispatch table is imported into every module as the first
      // table.
      getWasmTable(module);
    }
  }

  void output() {
    for (int i = 0; i < _table.length; i++) {
      Reference? target = _table[i];
      if (target != null) {
        w.BaseFunction? fun = translator.functions.getExistingFunction(target);
        // Any call to the dispatch table is guaranteed to hit a target.
        //
        // If a target is in a deferred module and that deferred module hasn't
        // been loaded yet, then the entry is `null`.
        //
        // Though we can only hit a target if that target's class has been
        // allocated. In order for the class to be allocated, the deferred
        // module must've been loaded to call the constructor.
        if (fun != null) {
          final targetModule = translator.moduleForReference(target);
          if (translator.isMainModule(targetModule)) {
            _definedWasmTable.setElement(i, fun);
          } else {
            // This will generate the imported table if it doesn't already
            // exist.
            (getWasmTable(targetModule) as w.ImportedTable).setElements[i] =
                fun;
          }
        }
      }
    }
  }
}

/// Build a row-displacement table based on fitting the [rows].
///
/// The returned list is the resulting row displacement table with `null`
/// entries representing unused space.
///
/// The offset of all [Row]s will be initialized.
List<V?> buildRowDisplacementTable<V extends Object>(List<Row<V>> rows,
    {int firstAvailable = 0}) {
  final table = <V?>[];
  for (final row in rows) {
    final values = row.values;
    int offset = firstAvailable - values.first.index;
    bool fits;
    do {
      fits = true;
      for (final value in values) {
        final int entry = offset + value.index;
        if (entry >= table.length) {
          // Fits
          break;
        }
        if (table[entry] != null) {
          fits = false;
          break;
        }
      }
      if (!fits) offset++;
    } while (!fits);
    row.offset = offset;
    for (final (:index, :value) in values) {
      final int tableIndex = offset + index;
      while (table.length <= tableIndex) {
        table.add(null);
      }
      assert(table[tableIndex] == null);
      table[tableIndex] = value;
    }
    while (firstAvailable < table.length && table[firstAvailable] != null) {
      firstAvailable++;
    }
  }
  return table;
}

class Row<V extends Object> {
  /// The values of the table row, represented sparsely as (index, value) tuples.
  final List<({int index, V value})> values;

  /// The given [values] must not be empty and should be sorted by index.
  Row(this.values) {
    assert(values.isNotEmpty);
    assert(() {
      int previous = values.first.index;
      for (final value in values.skip(1)) {
        if (value.index <= previous) return false;
        previous = value.index;
      }
      return true;
    }());
  }

  /// The selected offset of this row.
  late final int offset;

  int get width => values.last.index - values.first.index + 1;
  int get holes => width - values.length;
  int get density => (100 * values.length) ~/ width;
  int get sparsity => 100 - density;
}
