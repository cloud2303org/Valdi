//
//  File.swift
//
//
//  Created by Simon Corsin on 1/24/20.
//

import Foundation

class JavaScriptPreCompilerProcessor: CompilationProcessor {

    var description: String {
        return "Pre-compiling JavaScript files"
    }

    private let logger: ILogger
    private let fileManager: ValdiFileManager
    private let toolboxExecutable: ToolboxExecutable
    private let diskCache: DiskCache?
    private let androidJsBytecodeFormat: String?

    init(logger: ILogger,
         fileManager: ValdiFileManager,
         diskCacheProvider: DiskCacheProvider,
         toolboxExecutable: ToolboxExecutable,
         androidJsBytecodeFormat: String?) throws {
        self.logger = logger
        self.fileManager = fileManager
        self.toolboxExecutable = toolboxExecutable
        self.androidJsBytecodeFormat = androidJsBytecodeFormat

        if diskCacheProvider.isEnabled() {
            let versionString = try toolboxExecutable.getVersionString()
            self.diskCache = diskCacheProvider.newCache(cacheName: "js_precompiler", outputExtension: "jsbin", metadata: [
                "toolbox_version": versionString,
                "android_js_bytecode_format": androidJsBytecodeFormat ?? ""
            ])
        } else {
            self.diskCache = nil
        }

    }

    /// Hermes bytecode also goes to iOS: a Hermes runtime runs it there too, and release builds only link the lean
    /// Hermes VM, which can't compile source. JavaScriptCore and QuickJS bytecode formats keep shipping source to iOS.
    private var precompilesForIOS: Bool {
        return androidJsBytecodeFormat == "hermes"
    }

    private func precompile(item: SelectedItem<JavaScriptFile>, engine: String) throws -> Data {
        let jsPath = item.data.relativePath
        logger.debug("-- Pre-compiling JavaScript file \(jsPath)")

        let data = try item.data.file.readData()

        if let cachedData = diskCache?.getOutput(item: jsPath, platform: .android, target: .release, inputData: data) {
            return cachedData
        }

        let inputFile = URL.randomFileURL(extension: "js")
        let outputFile = URL.randomFileURL(extension: "js")
        try fileManager.save(data: data, to: inputFile)
        defer {
            _ = try? fileManager.delete(url: inputFile)
            _ = try? fileManager.delete(url: outputFile)
        }

        // Remove .js suffix, JS files are referred without their extension
        // in the consumer code
        var filename = jsPath
        if filename.hasSuffix(".js") {
            filename.removeLast(3)
        }

        try toolboxExecutable.precompile(inputFilePath: inputFile.path, outputFilePath: outputFile.path, filename: filename, engine: engine)

        let outputData = try File.url(outputFile).readData()
        try diskCache?.setOutput(item: jsPath, platform: .android, target: .release, inputData: data, outputData: outputData)
        return outputData
    }

    private func compile(item: SelectedItem<JavaScriptFile>) -> [CompilationItem] {
        var output = [CompilationItem]()
        let toAndroid = item.item.shouldOutputToAndroid
        let iosBytecode = precompilesForIOS && item.item.shouldOutputToIOS
        if item.item.shouldOutputToIOS && !iosBytecode {
            output.append(item.item.with(newPlatform: .ios))
        }
        if item.item.shouldOutputToWeb {
            output.append(item.item.with(newPlatform: .web))
        }

        if item.item.outputTarget.contains(.debug) {
            if iosBytecode {
                output.append(item.item.with(newKind: item.item.kind, newPlatform: .ios, newOutputTarget: .debug))
            }
            if toAndroid {
                output.append(item.item.with(newKind: item.item.kind, newPlatform: .android, newOutputTarget: .debug))
            }
        }

        guard let androidJsBytecodeFormat = self.androidJsBytecodeFormat else {
            if toAndroid {
                output.append(item.item.with(newKind: item.item.kind, newPlatform: .android, newOutputTarget: .release))
            }
            return output
        }

        do {
            let outputJsFile = item.data.with(file: .data(try precompile(item: item, engine: androidJsBytecodeFormat)))
            if toAndroid {
                output.append(item.item.with(newKind: .javaScript(outputJsFile), newPlatform: .android))
            }
            if iosBytecode {
                output.append(item.item.with(newKind: .javaScript(outputJsFile), newPlatform: .ios, newOutputTarget: .release))
            }
            return output
        } catch let error {
            logger.error(error.legibleLocalizedDescription)
            return [item.item.with(error: error)]
        }
    }

    func process(items: CompilationItems) throws -> CompilationItems {
        return items.select { (item) -> JavaScriptFile? in
            let precompiled = item.shouldOutputToAndroid || (self.precompilesForIOS && item.shouldOutputToIOS)
            if !precompiled || item.bundleInfo.compilationModeConfig.resolveCompilationMode(forItemPath: item.relativeProjectPath) == .js || !item.outputTarget.contains(.release) {
                return nil
            }

            switch item.kind {
            case .javaScript(let file):
                return file
            default:
                return nil
            }
        }.transformEachConcurrently(compile)
    }

}
